# Push-to-Talk Local Dictation Daemon (C, Linux/X11)

## Context

The goal is a fully local, offline push-to-talk dictation tool: hold a hotkey, speak, release, and the transcribed (optionally LLM-cleaned) text is typed into whatever window currently has focus, as if a human had typed it. No cloud APIs, no network dependency, no scripting-language orchestration layer — the entire application is C, linking directly against whisper.cpp's and (later) llama.cpp's clean C APIs (`whisper.h`, `llama.h`) as libraries rather than shelling out to subprocesses or an HTTP service. whisper.cpp and microui are already vendored in the working directory (`whisper.cpp/` at `v1.9.1-81-g6fc7c33b`, `microui/` at `v2.02`, both MIT).

Environment confirmed on this machine: Ubuntu 24.04, X11 session (`XDG_SESSION_TYPE=x11`), NVIDIA GTX 1650 4GB present but no CUDA toolkit installed, Ryzen 5 3550H (8 threads, AVX2), PipeWire as the running audio server (with ALSA compatibility), `cmake` not installed, `libxtst-dev`/`libevdev-dev` not installed, user not currently in the `input` group.

Design principle driving several decisions below: the tool is **focus-preserving by construction** — any optional status GUI must never steal input focus, because the entire point is that injected keystrokes land in the app the user was actually looking at, not in the tool's own window.

## Locked-in architecture decisions

1. **100% C application code.** whisper.cpp/llama.cpp are C++/CUDA internally but expose clean C headers — link against them via their C API only. microui is already pure C.
2. **CPU-only** for both whisper.cpp and llama.cpp (no `GGML_CUDA`, no `GGML_VULKAN`, no `WHISPER_SDL2`). The Ryzen's AVX2 handles short PTT clips on `tiny.en`/`base.en` near-instantly; GPU can be added later without a redesign.
3. **LLM cleanup via llama.cpp, not Ollama** (Phase B). Ollama is an external Go daemon requiring HTTP+JSON glue in C, which would outsource the core function of the module to an opaque black-box process — contradicting the "pure C, integrated" goal. llama.cpp shares whisper.cpp's `ggml` backend, author, and GGUF model format, and exposes a direct C API (`llama.h`).
4. **PTT hotkey via raw evdev** (`/dev/input/eventX`), not `XGrabKey`. Chosen for robustness (immune to X grab conflicts, clean down/up/repeat semantics) over the zero-setup alternative. Raw `struct input_event` reads (not `libevdev`, which isn't installed and isn't needed for ~50-80 lines of capability-probing code) — keeps dependencies minimal.
5. **Keystroke injection via X11 XTest** (`libxtst`, not yet installed).
6. **Audio capture via ALSA** direct PCM (not SDL2 — avoids pulling in whisper.cpp's SDL2-based example dependency; PipeWire's ALSA-compat layer makes this transparent).
7. **GUI backend: plain Xlib + software rendering**, not SDL2/OpenGL (microui's bundled demo approach) — reuses the same X connection already required for XTest injection/evdev is separate, avoiding a second graphics stack. Text rendering via Xlib core fonts (`XLoadQueryFont`/`XTextWidth`/`XDrawString`) — zero embedded assets needed.
8. **Two-thread model**: one thread owns the X11 `Display` connection (XTest injection +, if `--gui`, the microui event loop and rendering); a worker thread owns evdev listening, ALSA capture, whisper/llama inference. Handoff via a mutex+condvar-guarded struct; because the GUI thread blocks in `XNextEvent`/`select()`, the worker must also signal via a **self-pipe** (or `eventfd`) included in the GUI thread's `poll()` set — a plain shared flag will not wake it. `mu_Context` is touched exclusively by the GUI thread (microui is not thread-safe).
9. **Config**: hand-parsed `key=value` INI-style file, no JSON/TOML library.
10. **Build**: top-level `Makefile` drives whisper.cpp's (and later llama.cpp's) own CMake as sub-builds, plus `gcc` for the project's own sources and `microui/src/microui.c`.
11. **Single binary.** CLI/headless is the default mode; `--gui` is a runtime flag on the same process — no IPC between separate GUI/CLI binaries. *(Amended in Phase D: the setup GUI ships as a separate `dictation-setup` executable, because `pgrep -x dictation` process identity cannot distinguish a `--setup` window from a running daemon. The no-IPC rationale still holds — the setup window talks to the daemon only via `scripts/waybar-dictation.sh`. See Phase D.)*
12. **Models**: whisper `tiny.en`/`base.en` (via whisper.cpp's own `models/download-ggml-model.sh`); llama.cpp Phase B uses a small quantized instruct GGUF (e.g. Qwen2.5-1.5B-Instruct or Llama-3.2-1B-Instruct, Q4_K_M).

**This file is the single living source of truth across all phases** — phase "done" checklists, verification notes, and any deviations discovered during implementation get appended to this same document in place. No separate `PHASE_A.md`/`PHASE_B.md` files, to avoid drifting/inconsistent assumptions across phases.

## Module breakdown

```
voice/
├── PLAN.md                  # living plan — this content, updated in place per phase
├── Makefile
├── configs/example.conf     # sample key=value config
├── models/                  # gitignored; downloaded whisper/llama GGUF files
├── whisper.cpp/             # vendored, unmodified except build output dirs
├── llama.cpp/                # vendored in Phase B
├── microui/                  # vendored, unmodified
├── include/                  # shared cross-module constants if needed
└── src/
    ├── main.c                # arg parsing, config load, thread spawn/join, signal handling
    ├── config.h / config.c   # INI parser -> struct app_config; defaults; validation
    ├── hotkey_evdev.h / .c   # device auto-detect/open, PTT down/up detection, --list-keys mode
    ├── audio_alsa.h / .c     # ALSA PCM open/start/stop, growing float32 buffer, format conversion
    ├── stt_whisper.h / .c    # whisper_context lifecycle, whisper_full() wrapper, segment concat
    ├── llm_cleanup.h / .c    # (Phase B) llama context lifecycle, prompt templating, cleanup call
    ├── inject_xtest.h / .c   # shared X11 Display, keysym/keycode+shift synthesis, typing loop
    ├── gui_xlib.h / .c       # (Phase C) override-redirect window, event loop, mu_input_* feed, render
    ├── font_xlib.h / .c      # (Phase C) XLoadQueryFont wrapper for microui text_width/text_height
    ├── ipc_handoff.h / .c    # (Phase C) mutex+condvar struct + self-pipe for GUI-thread wakeup
    ├── app_state.h           # shared enum: IDLE/RECORDING/TRANSCRIBING/CLEANING/INJECTING/ERROR
    └── log.h / .c            # tiny leveled logging to stderr
```

**Key function-level responsibilities:**
- `hotkey_evdev.c`: `hotkey_find_keyboard_device()` scans `/proc/bus/input/devices`/`EVIOCGBIT` for a real keyboard (this machine has ~19 event nodes including decoys — HDMI jack-sense, vendor hotkey devices — so `ptt_device` must always be settable as an explicit config override). `hotkey_run(cfg, on_down, on_up)` filters to the configured evdev code, discards `value==2` (auto-repeat), treats `1`=down/`0`=up. `--list-keys` CLI mode prints `(device, code, name)` for every keypress so the user can fill in the config without guessing — **this must ship with Phase A**, not as an afterthought, since Phase A's manual verification depends on it.
- `audio_alsa.c`: mono 16kHz capture; convert `S16_LE` to normalized float32 `[-1,1]` (or use `FLOAT_LE` directly if the PipeWire-ALSA-compat stack supports it — verify empirically in Phase A and record the answer here). Growing realloc buffer per utterance; no sliding-window/streaming logic (PTT clips are single-shot).
- `stt_whisper.c`: `whisper_init_from_file_with_params(..., use_gpu=false)`; `whisper_full()` with `whisper_full_default_params(WHISPER_SAMPLING_GREEDY)`, `language="en"`; concatenate `whisper_full_get_segment_text()` across `whisper_full_n_segments()`.
- `inject_xtest.c`: owns the shared `Display*`. Per character: resolve `KeySym` → `XKeysymToKeycode`; if Shift is required (uppercase, shifted punctuation), bracket with `XTestFakeKeyEvent` Shift press/release, restoring modifier state afterward so nothing is left "stuck." This is real work, not "send a string."
- `ipc_handoff.c` (Phase C): mutex+condvar struct (`app_state`, `last_transcript`, `pending_inject_text`) plus a self-pipe byte-write to unblock the GUI thread's `poll()`.
- `config.c`: fields — `whisper_model_path`, `llama_model_path` (optional), `ptt_device` (optional override), `ptt_keycode` (evdev code — **explicitly not** an X keysym/keycode; document this distinction everywhere both appear near each other), `audio_device` (optional override, mirrors `ptt_device` — auto-detect can pick the wrong ALSA capture card if more than one is present, e.g. a USB webcam mic), `n_threads`, `language` (fixed `"en"`), `test_mode`, `gui_enabled`.

## Threading and data flow

**Headless (no `--gui`):** main thread inits everything and spawns the worker thread, then joins. Worker thread: evdev read-loop → on PTT-down start ALSA capture → on PTT-up stop capture → `stt_transcribe()` → (Phase B) `llm_clean()` → if `test_mode` print to stdout, else call `inject_type_text()` directly (same thread, no handoff needed).

**GUI mode (`--gui`):** main thread becomes the X/GUI thread, looping `poll()` over `{ConnectionNumber(display), self_pipe_fd}` — on X activity, dispatch to `mu_input_*`/redraw; on self-pipe activity, read `ipc_handoff` under the mutex and both update the status label and perform the injection. Worker thread runs the identical pipeline as headless but writes results into `ipc_handoff` + signals the self-pipe instead of injecting directly. `mu_Context` is never touched from the worker thread.

## One-time system setup

```
sudo apt update
sudo apt install -y cmake libxtst-dev
sudo usermod -aG input $USER   # then log out/in (newgrp does not propagate to GUI re-login)
# alternative to the group approach: a udev rule in /etc/udev/rules.d/ granting
# `input`-group read access, then `sudo udevadm control --reload-rules && sudo udevadm trigger`
./whisper.cpp/models/download-ggml-model.sh base.en   # or tiny.en for lower latency
```
Already present: build-essential (gcc/g++ 13.3.0), libasound2-dev, libx11-dev, libxi-dev.

**Mic gain check (required, not optional)** — found empirically on this machine (see Progress log): out-of-the-box ALSA `Capture`/`Mic Boost` levels can be maxed out (+30dB each), which clips 100% of captured audio regardless of application code. Before relying on dictation quality, record a few seconds and check the signal isn't pinned at full scale for the whole clip (peak/RMS near 1.0 throughout, not just on loud transients):
```
arecord -d 3 -f S16_LE -r 16000 -c 1 /tmp/check.wav   # then inspect, or just judge by ear via aplay
amixer -c 1 sset 'Mic Boost' 0        # card/control names vary -- `amixer -c <N> scontrols` to list
amixer -c 1 sset 'Capture' 40%
sudo alsactl store                     # persist at the ALSA layer
```
**Caveat**: WirePlumber (PipeWire's session manager, confirmed running on this machine) may manage its own remembered volume per device independently of raw ALSA mixer state, and could re-raise capture gain on next login regardless of `alsactl store`. If dictation quality degrades after a reboot, re-check levels the same way rather than assuming the one-time fix persisted.

## Makefile design

Targets: `deps-whisper` (CMake sub-build: `-DBUILD_SHARED_LIBS=OFF -DWHISPER_SDL2=OFF -DGGML_CUDA=OFF -DGGML_VULKAN=OFF`, no-op if already built), `deps-llama` (Phase B, same pattern), `app` (gcc compiles `src/*.c` + `microui/src/microui.c`, links `-lwhisper -lasound -lX11 -lXtst -lXi -lpthread -lm`, Phase B adds `-lllama`), `all`, `clean` (project artifacts only — does not nuke the slow-to-rebuild vendored `build/` dirs), `run`, `list-keys`.

## Phased implementation

### Phase A — Core pipeline (capture → whisper → inject, headless, test-mode capable)
Files: `config.c`, `hotkey_evdev.c` (+ `--list-keys`), `audio_alsa.c`, `stt_whisper.c`, `inject_xtest.c`, `main.c`, `log.c`. No llama.cpp, no GUI.
1. System setup (above). Build whisper.cpp via `make deps-whisper`.
2. Implement + validate `stt_whisper.c` in isolation against the already-vendored `whisper.cpp/samples/jfk.wav` — decode PCM, call `stt_transcribe()`, confirm sane output. De-risks whisper integration before touching hardware-dependent code.
3. Implement `hotkey_evdev.c` + `--list-keys`; verify correct evdev code/device printed on keypress, and that auto-repeat doesn't spam extra events.
4. Implement `audio_alsa.c`; verify a captured clip is audible/sane (write to a raw/WAV file) before wiring to whisper.
5. Implement `inject_xtest.c`; verify exact fidelity typing a known mixed-case/punctuation string into a focused editor; verify `test_mode=true` prints instead of injecting.
6. Wire all four into `main.c`. **End-to-end manual test**: run with `test_mode=false`, focus a text editor, hold PTT, speak, release, confirm correctly-cased text appears in the editor within a few seconds. Also verify `test_mode=true` prints instead of injecting.

Done when: all steps above pass manually; binary runs as a controllable background process via config + `--config`/`--test-mode`/`--list-keys` flags.

### Phase B — LLM cleanup via llama.cpp
1. `git clone https://github.com/ggml-org/llama.cpp` (sibling to `whisper.cpp/`), pin and record the commit here.
2. `make deps-llama` (same CPU-only CMake flags as whisper.cpp).
3. Download a small quantized instruct GGUF (Qwen2.5-1.5B-Instruct or Llama-3.2-1B-Instruct, Q4_K_M) into `models/`.
4. Implement `llm_cleanup.c`: one fixed prompt template ("fix punctuation/casing, don't add content"), single greedy decode, stateless per utterance.
5. Verify in isolation against a few hand-typed raw transcripts before wiring live.
6. Wire into `main.c` as a **config toggle** (`llama_model_path` unset → behavior identical to Phase A raw whisper output) — not a hard requirement, since whisper's English output already has reasonable punctuation/casing on its own.
7. End-to-end manual test with cleanup enabled; measure and record added latency here.

Done when: cleanup is a working, toggleable stage; both toggle states verified; llama.cpp commit recorded.

### Phase C — microui/Xlib status GUI
1. Implement `ipc_handoff.c` (mutex+condvar + self-pipe), verified standalone that a byte write reliably wakes a `poll()` on another thread.
2. Implement `font_xlib.c` (`XLoadQueryFont`/`XTextWidth` satisfying microui's `text_width`/`text_height` callbacks).
3. Implement `gui_xlib.c`: override-redirect window (`override_redirect=True` — the mechanism guaranteeing no focus-stealing), X-event→`mu_input_*` translation, `mu_next_command` dispatch (`MU_COMMAND_RECT`→`XFillRectangle`, `TEXT`→`XDrawString`, `CLIP`→`XSetClipRectangles`), `poll()` loop merging `ConnectionNumber(display)` with the self-pipe fd.
4. Rewire `main.c` per the GUI-mode threading section above.
5. Manual verification, in order: (a) status window appears, never requires reclaiming focus; (b) focus a separate editor, do a PTT dictation, confirm the status label progresses through states AND the text lands in the editor AND the editor stays focused throughout (the explicit focus-non-theft check); (c) `gui_enabled=false` still works unchanged (regression check).

Done when: both modes work from the same binary via one runtime flag; focus-preservation check passes; code review confirms `mu_Context` is touched only from the GUI thread.

### Phase D — Setup GUI, model downloader, desktop integration

**Motivation.** Going from `git clone` to a working daemon is currently a manual, README-driven
process: download a whisper model via `download-ggml-model.sh`, `wget` a GGUF cleanup model, then
hand-edit `configs/example.conf` to point `whisper_model_path`/`llama_model_path` at what landed.
Every step is a place to get a path wrong, and `config_validate()` turns a wrong path into a fatal
startup error with no guidance. Phase D replaces that with a small microui/Xlib **setup window**:
pick a whisper model and a cleanup model from a list, download whatever is missing (live progress
bar + the raw `curl` output visible in-window), press **Start**. Plus an XDG `.desktop` entry so
the tool appears in the desktop app search menu like any other installed application.

**The Super+D keybind is unchanged.** It calls `scripts/waybar-dictation.sh toggle` directly and
never opens the GUI; the setup window and the keybind are independent front-ends onto the same
script.

**Deviation from decision #11 (recorded deliberately).** Phase D ships a **second binary**,
`dictation-setup`, rather than a `--setup` flag on `dictation`. The flag approach was chosen first
and is broken: the launcher script's whole process-identity contract is `pgrep -x dictation` /
`pkill -INT -x dictation`, which matches the *executable basename* and ignores arguments. A window
running as `dictation --setup` is therefore indistinguishable from a running daemon — `is_running()`
returns true the moment the window opens, so `[Start]` silently no-ops forever, and `[Stop]` (or
Super+D while the window is open) would kill the setup window instead of the daemon. A distinct
process name removes this by construction with no script changes. Note that #11's stated rationale
("no IPC between separate GUI/CLI binaries") does not apply here: the setup window performs no IPC
with the daemon, it invokes the same launcher script Waybar does.

**Enabling refactor (D0).** `config.c` includes `llm_cleanup.h` for a single call to
`llm_style_is_known()`, which drags `llm_cleanup.o` → `libllama.a` → `libggml*.a` → CUDA — the
reason every `tests/` binary is 73 MB. `nm -u src/config.o` confirms that symbol is the *only*
non-libc, non-`log_msg` undefined reference, so moving the `{name, system_prompt}` style table into
a new dependency-free `src/llm_styles.c` (~15 lines) severs the edge. `dictation-setup` then links
only `-lX11` and builds **without whisper, llama, or CUDA present at all** — so on a fresh clone the
setup GUI can be built in seconds and used to download models *before* the ~10-minute dependency
build.

**Locked-in Phase D decisions:**

- **Downloads shell out to `curl`, not libcurl.** `fork`/`execvp("curl", …)` with stdout+stderr
  merged onto one pipe, streamed line-by-line into a scrollable log pane in the window. Chosen so
  failures show the *actual* command output rather than a paraphrase — a vague "download failed" is
  useless for diagnosing what upstream changed. The exact command is echoed as the pane's first line
  so it can be pasted into a terminal to reproduce. This also keeps `LDLIBS` free of `-lcurl`,
  preserving the "no network code in the binary" property stated at the top of this document.
- **`--fail` is mandatory** on the curl invocation. Without it an HTTP 404 writes the error page
  into the output file, leaving a ~500-byte "model" that then fails incomprehensibly deep inside
  whisper.cpp. Download to `<dest>.part` and `rename()` only on exit 0, so a partial or failed
  fetch never occupies the final filename.
- **No threads.** The download is a child process; its pipe fd joins `ConnectionNumber(dpy)` in the
  existing `poll()` skeleton. The only change from the status panel's loop is that the infinite
  timeout becomes ~250 ms while a download or daemon-status poll is live, since the progress bar is
  computed by re-`stat()`ing the `.part` file and must tick even when curl emits nothing.
- **Model catalog lives in `configs/models.conf`**, not a hardcoded C table — pipe-separated
  `kind|id|display|filename|url|size_bytes` records. When HuggingFace moves a file, that is a
  one-line data edit rather than a recompile. Sizes are advisory (progress bar only); a stale size
  makes the bar slightly wrong but never breaks a download.
- **Config is written to a new gitignored `configs/local.conf`**, seeded by copying
  `example.conf` (so it inherits the ~55 lines of comments and every other tuned setting). The
  writer is **line-preserving** — the parser in `config.c` discards comments, so a naive
  read-modify-write would destroy the documentation. Write to `.tmp` + `rename()`.
  `waybar-dictation.sh` and `main.c`'s default `config_path` both prefer `local.conf` when present.
- **Start/stop shells out to `scripts/waybar-dictation.sh`**, never reimplemented in C. That script
  already handles the mandatory `cd "$DICT_DIR"` (relative model paths), the nohup detach, the log
  file, and the Waybar signal; a second launcher could disagree with it about process identity.
- **Start is gated** on the selected whisper model actually being present. `local.conf` is seeded
  from `example.conf`, which names models that don't exist on a fresh clone — an ungated Start would
  hand the user the exact `config_validate` fatal this phase exists to eliminate, just behind a
  button. A cleanup model stays optional (blank `llama_model_path` disables cleanup at runtime) and
  must not gate Start.
- **No editable text fields in the window.** Deliberate scope limit: it avoids `XLookupString` +
  `mu_input_text` + `mu_textbox` plumbing entirely (the status panel has no keyboard path at all
  today) and is what keeps the window genuinely minimal. Everything is list selection and buttons.

**Inverted from the Phase C panel** (each of these is load-bearing for the panel and wrong here):
`override_redirect` stays **False** so the window is WM-managed and focusable; `WM_DELETE_WINDOW`
must be handled or clicking the WM close button drops the X connection and Xlib aborts the process;
`KeyPressMask` is added (Esc-to-close only); `XSetClassHint` `res_class` must equal the
`.desktop` file's `StartupWMClass` or the window won't associate with its launcher icon; font
defaults to `9x15` rather than `fixed` (13px is cramped for a widget-dense form). `gui_xlib.c` is
**not modified** — `setup_gui.c` reuses its rasterizer, color cache, backbuffer blit and poll
skeleton by copy, so the focus-preserving panel cannot regress.

Sub-phases, each independently verifiable: **D0** llm_styles split → **D1** catalog + config writer
(+ `tests/test_catalog.c`, `tests/test_config_write.c`) → **D2** downloader, exercised first via a
headless `--fetch-model <id>` flag so the fork/exec/pipe/CR-translation work is de-risked before any
GUI exists (same discipline as validating `stt_whisper.c` standalone in Phase A) → **D3** the window
→ **D4** daemon start/stop wiring → **D5** `.desktop` + `make install-desktop`.

**Deferred to future work:** pinning HuggingFace URLs to a commit revision instead of `main`,
querying the HF JSON API for filenames rather than hardcoding them, and SHA256 verification of
downloads. The catalog-as-data decision above is the cheap mitigation that covers most URL rot;
these three are the fuller fix if breakage actually becomes routine.

Done when: `dictation-setup` builds without the CUDA/llama chain; a fresh clone can select and
download both models and start the daemon entirely from the window; `pgrep -x dictation` prints
nothing while the setup window is open; Super+D and `./dictation --gui` both behave exactly as
before.

## Correctness-critical details (call out explicitly in code/comments where relevant)
- evdev `value==2` (auto-repeat) must never restart capture — only edges (`1`/`0`) matter.
- evdev keycodes and X keysyms/keycodes are different numberspaces — never conflate them in code, comments, or config docs.
- Multi-keyboard ambiguity is real on this machine (2 legitimate keyboard devices + several `EV_KEY`-exposing decoys) — auto-detect is a heuristic, `ptt_device` override must always work.
- A shared-memory flag alone cannot wake a thread blocked in `XNextEvent`/`select()` — the self-pipe/`eventfd` is required, not optional.
- GUI window must never call any focus-requesting API (`XSetInputFocus`, focus-raising `XMapRaised`, etc.).

## Verification
Each phase ends with an explicit end-to-end manual test (described per-phase above) run by the user: hold the configured PTT key while focused on a real text editor, speak, release, and confirm the correct text is typed into that editor (or printed, in test-mode) without the editor ever losing focus. Phase A/B use headless mode; Phase C additionally confirms the status GUI never steals focus. Latency and any environment-specific findings (ALSA format choice, keyboard device ambiguity resolution, model latency numbers) get recorded directly in this file as they're discovered.

## Progress log

### Phase D0 — llm_styles split (done)

- **`src/llm_styles.{h,c}`** created: the `struct cleanup_style` definition, the three-row
  `STYLES[]` table, `llm_style_find()` (was the file-static `find_style()`), and
  `llm_style_is_known()` moved verbatim out of `llm_cleanup.c`. Prompts unchanged.
- **Callers repointed**: `llm_cleanup.c` includes `llm_styles.h` and calls `llm_style_find()`;
  `config.c:2` swapped `llm_cleanup.h` → `llm_styles.h`; `llm_style_is_known`'s declaration moved
  out of `llm_cleanup.h`. `src/llm_styles.c` added to `SRCS`. No other call sites existed
  (grep-confirmed: only `config.c:153` ever used it).
- **Decoupling verified**: `nm -u src/llm_styles.o` reports exactly one undefined symbol,
  `strcmp`. `config.o`'s undefined set is otherwise unchanged and now libc-only plus `log_msg`
  and `llm_style_is_known`.
- **Payoff measured, not assumed**: a probe calling `config_load()` links cleanly from just
  `config.o + log.o + llm_styles.o` with **no `-lllama`, `-lwhisper`, `-lggml*`, `-lcudart`,
  `-lX11` or `-lpthread`**, and parses `example.conf` correctly. **17,640 bytes** versus
  **73,321,952** for `tests/test_config` — ~4,000x smaller. This is what lets `dictation-setup`
  (D1-D5) build without the CUDA/llama toolchain present.
- **No regression**: `make app` builds warning-clean apart from the pre-existing
  `hotkey_evdev.c` `-Wformat-truncation`. `make test` exits 0 — `test_config`, `test_directives`,
  `test_ipc`, `test_llm`, `test_inject` all pass; `test_stt` SKIPs because it hardcodes
  `models/ggml-base.en.bin`, which is absent on this machine (only the large-v3-turbo variants
  are downloaded). That skip predates this change and is unrelated to it.

### Enhancement — whisper on GPU (done)

- **Motivation**: after Phase B, transcription (CPU) was the largest latency chunk while the LLM cleanup already ran on GPU. Moved whisper to the GTX 1650 too.
- **Nearly free, no whisper rebuild**: whisper.cpp selects its GPU backend at *runtime* via the ggml device registry (`whisper_backend_init_gpu`), not at compile time. Because the app already links llama's CUDA-enabled ggml 0.15.3, whisper only needed `use_gpu=true` to discover the CUDA0 device — confirmed in the log: `whisper_model_load: CUDA0 total size = 147.37 MB`, `whisper_backend_init_gpu: using CUDA0 backend`. The whisper.cpp CMake build stayed CPU-only (`-DGGML_CUDA=OFF`); it doesn't matter since we link llama's ggml.
- **Change**: `stt_init` gained a `bool use_gpu` param (sets `cparams.use_gpu`/`gpu_device`); new `whisper_use_gpu` config key (default `false` in code, `true` in `example.conf`); callers in `main.c` + `tests/test_stt.c` updated (test defaults to GPU, `DICT_TEST_GPU=0` forces CPU). No Makefile change.
- **Latency (11 s jfk.wav, `test_stt` wall incl. one-time model load)**: CPU ~2.3-2.4 s → GPU ~1.0-1.3 s. Per-utterance transcription in the daemon (model loaded once) is faster still since load is amortized. Both models resident together: whisper 147 MB + Qwen ~940 MB ≈ 1.1 GB of the 4 GB card; no contention (sequential transcribe→clean). `test_stt` still passes on the GPU path (correctness through the shared ggml). Graceful CPU fallback if no GPU (`whisper_use_gpu=false` verified identical to before).
- Optional future: `flash_attn=true` (extra Turing speedup); building whisper itself with CUDA is unnecessary given the shared ggml.

### Phase B — done

- **Compute decision: GPU.** CPU cleanup on the Ryzen 5 3550H was estimated at ~3-7 s/utterance; the GTX 1650 (4 GB) runs Qwen2.5-1.5B Q4_K_M in ~1-1.5 s incl. one-time load. User chose GPU from the start.
- **llama.cpp pinned + shared-ggml strategy (the crux).** Vendored `llama.cpp` as flat source, pinned to **`96183e982`** ("ggml : bump version to 0.15.3") — the exact commit whose bundled ggml (**0.15.3**) matches whisper.cpp's, AND which supports the `qwen2` arch (`LLM_ARCH_QWEN2`, `src/models/qwen2.cpp`). whisper.cpp and llama.cpp each vendor their own ggml; with static libs you can't link two. **Resolution: link llama's CUDA-enabled ggml 0.15.3 for BOTH** (`-lggml -lggml-base -lggml-cpu -lggml-cuda` from `llama.cpp/build/`, wrapped in `--start-group`; drop whisper's ggml `-L`). whisper's CPU path uses the CPU backend inside that same ggml (`use_gpu=false`); no whisper rebuild needed. The app binary is ~200 MB (CUDA kernels statically linked).
- **CUDA toolkit**: `nvidia-cuda-toolkit` **12.0.140** installed via apt (driver 595 / CUDA 13.2 runs the 12.x toolkit fine — forward-compatible). `deps-llama` builds with `-DGGML_CUDA=ON -DCMAKE_CUDA_ARCHITECTURES=75 -DLLAMA_CURL=OFF` (sm_75 = GTX 1650; single arch keeps the build small). Built `-j4` to stay within ~8 GB free RAM (parallel nvcc is memory-heavy).
- **ggml/CUDA de-risk gate PASSED (all four):** (a) app links clean — no duplicate/missing ggml symbols, CUDA libs (`-lcudart -lcublas -lcuda`) resolve; (b) **`test_stt` still passes** — whisper transcribes jfk.wav correctly through llama's shared ggml (the ABI-compatibility proof); (c) Qwen loads + greedy-decodes; (d) `offloaded 29/29 layers to GPU`, `CUDA0 model buffer = 934.70 MiB` — model is VRAM-resident.
- **`llm_cleanup.{h,c}`** (mirrors `stt_whisper`): `llm_init`/`llm_clean`/`llm_free`. Implemented against the pinned commit's `examples/simple/simple.cpp` + `simple-chat.cpp` + `include/llama.h`. Chat prompt via the model's own template (`llama_model_chat_template` + `llama_chat_apply_template`), greedy sampler, **stateless per utterance** (`llama_memory_clear` before each call), bounded output (`n_tok*2+32`). Best-effort: any failure returns NULL and the caller keeps the raw transcript, so cleanup never breaks dictation. Sampled-token var lives outside the decode loop (its address is held by `llama_batch_get_one`).
- **Configurable styles**: `dictation` (default) / `code` / `commands` as a `{name, system_prompt}` table selected by the `cleanup_style` config key (unknown → warn + fall back to `dictation`). New config keys: `cleanup_style`, `n_gpu_layers` (default 99). Wired at the pre-reserved CLEANING seam in `on_ptt_up`; GUI already renders `cleaning`.
- **Prompt-tuning finding**: Qwen2.5-1.5B initially added punctuation but *refused to capitalize* (treating casing as "content"). Fixed by making the `dictation` prompt explicitly require capitalizing sentence starts and "I", plus a **one-shot example** in the system prompt. Result: `"hello world how are you i am doing well today"` → `"Hello world. How are you? I am doing well today."`
- **ASCII-injection safety net (correctness)**: `inject_xtest.c` is ASCII-only and silently drops bytes >0x7E -- and an instruct model "fixing punctuation" is exactly what may emit curly quotes (U+2019 in `it's`), em-dashes, or ellipsis, which would corrupt the injected text (`it's`→`its`). `llm_clean` now runs a `normalize_ascii` pass mapping the common typographic offenders to ASCII (unit-verified on crafted input) before returning, guaranteeing faithful injection regardless of model output. (In practice Qwen emitted straight ASCII quotes on the phrases tried, but the net makes it robust.)
- **All three styles validated** (not just dictation): `dictation` → `He said, "Don't do that," and it's fine, okay.`; `code` (`def add a comma b return a plus b`) → `def add(a, b): return a + b`; `commands` (`list all files in the current directory`) → `ls -l`; unknown style falls back to `dictation`. Note: `code`/`commands` are intentionally transformative (they rewrite toward code/command form), per the chosen "configurable styles" scope.
- **`tests/test_llm.c`** added (skips if model absent): asserts content words preserved, first letter capitalized, terminal punctuation, no runaway. **All four tests pass** (`test_config`/`test_ipc`/`test_stt`/`test_llm`).
- **Daemon smoke test** (`--test-mode`, cleanup on): whisper (CPU) + Qwen (GPU, 934 MB VRAM) both load, ready, clean SIGTERM shutdown.
- **Latency**: `test_llm` total 1.53 s wall *including* one-time GPU model load; per-utterance cleanup (model stays loaded in the daemon) is sub-second.
- **Live mic verification (user-run) — passed**: PTT dictation with cleanup on produced cleaned, correctly-capitalized injected text (contraction apostrophes intact), and the GUI panel progressed `... → cleaning → injecting`. All three phases (A/B/C) are now complete.

### Phase C — done

- **Repo hardening (done first)**: top-level `git init` + baseline commit of the working Phase A tree. The vendored `whisper.cpp/` and `microui/` each carried their own `.git` (whisper's was 41 MB); removed and committed as flat vendored source (upstream tags recorded in `README.md`: whisper.cpp `v1.9.1-81-g6fc7c33b`, microui `0850aba`/v2.02). Added `README.md` (setup/build/run/config/privacy) and a `tests/` suite + `make test`.
- **`ipc_handoff.{h,c}`**: mutex + self-pipe (both ends `O_NONBLOCK`) + `enum app_state` + `pending_inject_text` + `last_transcript`. Worker publishes every state transition and signals the pipe; GUI polls the read end. Verified standalone (`tests/test_ipc.c`): a byte written on one thread reliably wakes a `poll()` on another, and state/inject-text transfer round-trips.
- **`font_xlib.{h,c}`**: `XLoadQueryFont` wrapper; the `XFontStruct*` is passed to microui *as* its opaque `mu_Font` (stored in `ctx->style->font`) so the `text_width`/`text_height` callbacks recover it by cast — no globals. Falls back to `"fixed"`. **Env finding**: default `"fixed"` on this machine reports ascent=11 descent=2.
- **`gui_xlib.{h,c}`**: `override_redirect=True` borderless window (top-right, 300×100), `XMapWindow` (never `XMapRaised`/`XSetInputFocus`). microui commands rasterized to a `Pixmap` backbuffer (`RECT`→`XFillRectangle`, `TEXT`→`XDrawString` at baseline `pos.y+ascent`, `CLIP`→`XSetClipRectangles`) then `XCopyArea`'d to the window; small mu_Color→pixel cache via `XAllocColor`. `poll()` loop merges `ConnectionNumber(dpy)` + the ipc self-pipe; injection is performed **on the GUI thread** (which owns `Display`), which then sets `IDLE`. `mu_Context` is touched only here.
- **`main.c` rewired**: pipeline refactored into one mode-parameterized path (`publish_state()` is a no-op headless, `ipc_set_state` in GUI mode). Headless path preserved exactly (Phase A behavior). GUI path: main thread becomes X/GUI thread, worker thread runs capture→stt and hands off via `ipc_post_inject`. An explicit **Phase B insertion point** for the `CLEANING`/`llm_clean` stage is marked in `on_ptt_up` — the handoff and GUI already carry `APP_STATE_CLEANING`, so Phase B drops in with no re-threading. Added `--gui` flag and optional `gui_font` config key.
- **Build**: `Makefile` now compiles `microui/src/microui.c` + the 3 new modules; no new system deps (Xlib core drawing covered by existing `-lX11`). Project sources build warning-clean (one pre-existing `-Wformat-truncation` warning remains in Phase A `hotkey_evdev.c`, untouched). `make test` (config parser, self-pipe handoff, whisper-on-jfk.wav) passes.
- **Smoke test (this machine, `--gui --test-mode`)**: whisper loaded → X display opened → `fixed` font loaded → override-redirect panel created → evdev auto-detected `/dev/input/event3` and listening (confirms the `input` group is now active post-relogin) → on SIGTERM the GUI `poll()` woke via the self-pipe, the loop exited, and cleanup ran in order. No crash, clean shutdown.
- **Manual verification (user-run) — both passed**: (a) the panel never requires reclaiming focus — dictated into a separate editor, its caret kept blinking; (b) live PTT dictation showed the label progress `idle→recording→transcribing→injecting→idle` while the text landed in the *focused editor*, which stayed focused throughout. (The `injecting` label render was fixed after an initial ordering bug where it was skipped — commit `72d21dc`.)

### Phase A — done

- **System setup**: `cmake` and `libxtst-dev` installed. `sudo usermod -aG input $USER` run; takes effect after full logout/login (`newgrp` only affects the shell it's run in directly, does not propagate to other processes or a re-login) — `sg input -c "..."` used to test/run evdev-dependent code in the meantime.
- **whisper.cpp built** (CPU-only static libs) via CMake with `-DBUILD_SHARED_LIBS=OFF -DWHISPER_SDL2=OFF -DGGML_CUDA=OFF -DGGML_VULKAN=OFF`. `base.en` model downloaded, symlinked at `models/ggml-base.en.bin`.
- **`stt_whisper.c` validated** against the vendored `whisper.cpp/samples/jfk.wav`: produced the exact expected JFK transcript. Confirms `whisper_init_from_file_with_params`/`whisper_full`/segment concatenation all work correctly with `use_gpu=false`.
- **`hotkey_evdev.c` validated**: device auto-detection correctly found a real keyboard (skipped decoy `EV_KEY` devices like jack-sense/HDMI nodes); `--list-keys` correctly reported `(device, code, name)` for a live keypress observed simultaneously on two keyboard devices (`event3` internal, `event17` external USB) while the user was typing in this chat — this incidentally confirms evdev sees raw hardware events regardless of window focus, exactly the property PTT needs. Auto-detected device on this machine: `/dev/input/event17` ("CASUE USB KB"). **Note**: `hotkey_run()` only listens on the single auto-detected device (unlike `--list-keys`, which scans all of them) — if the user's actual PTT key is on a different physical keyboard than the auto-detected one, `ptt_device` must be set explicitly in the config, otherwise presses on the other keyboard won't register (this bit us once during manual testing: a press on the built-in keyboard went unnoticed while the daemon listened only on the external USB one).
- **Environment finding — mic gain**: this machine's ALSA `Capture` and `Mic Boost` mixer controls (card 1, "HD-Audio Generic", ALC285) were both at 100% (+30dB each, 60dB combined) out of the box, causing sustained full-scale clipping on any capture. Lowered via `amixer -c 1 sset 'Mic Boost' 0` and `amixer -c 1 sset 'Capture' 40%`. Anyone deploying this on a fresh machine should sanity-check capture levels the same way (record a few seconds, check peak/RMS aren't pinned at 1.0 for the whole clip) rather than assuming `alsamixer` defaults are sane.
- **Environment finding — ALSA device (important)**: PipeWire's `"default"` ALSA node was found to **corrupt/clip 16kHz mono capture** on this machine regardless of mixer gain (verified byte-for-byte identical clipped output between our own capture code and the reference `arecord` tool against `-D default`, ruling out an application bug). Direct hardware capture (`hw:1,0`) was clean but only supports 44.1kHz stereo natively. `plughw:1,0` (ALSA's own plug/rate/channel conversion, bypassing PipeWire's routing/resampling entirely) produced clean 16kHz mono audio with zero clipping. `sysdefault` failed outright (`dsnoop` couldn't open the slave, PipeWire already owns the device). **Fix implemented in `audio_alsa.c`**: `find_capture_device()` enumerates ALSA cards via `snd_card_next`/`snd_ctl_pcm_info` to find the first capture-capable card and constructs `plughw:<card>,<dev>` dynamically (avoids hardcoding "card 1", falls back to `"default"` if no hardware card is found).
- **`inject_xtest.c` validated**: a standalone typing test (mixed case, digits, and shifted symbols `!-:+?#@%`) reproduced the exact input string when typed into a focused window, confirming the `XGetKeyboardMapping`-based Shift bracketing and Latin-1 keysym resolution work correctly.
- **Full end-to-end verified twice**: (1) `--test-mode` run — held PTT, said "I am hungry now.", released — exact transcript printed to stdout. (2) Real injection run (`test_mode=false`) — held PTT, spoke, released — user confirmed the transcribed text appeared exactly correctly in a focused text editor. Both runs went through the complete `hotkey_evdev` → `audio_alsa` → `stt_whisper` → (`inject_xtest` or stdout) pipeline as wired in `main.c`.
- Phase A is complete: `config.c`, `log.c`, `hotkey_evdev.c` (+ `--list-keys`), `audio_alsa.c`, `stt_whisper.c`, `inject_xtest.c`, `main.c` all implemented, compiling cleanly (`-std=gnu11`, needed because strict `-std=c11` conflicts with ALSA's `struct timespec` guard macros), and verified. Top-level `Makefile` builds whisper.cpp via CMake sub-build and links the app in one `make app` (or `make all`).
- **`audio_device` config field added**, mirroring `ptt_device`: `audio_capture_start()` now takes an optional device-string override so a machine with multiple capture-capable ALSA cards (e.g. a USB webcam mic) isn't stuck with whatever `find_capture_device()` picks first.
- **Latency measurement** (whisper `base.en` vs `tiny.en`, both CPU-only, on the 11s `jfk.wav` sample, `n_threads=4`): `base.en` = 3.7s wall time (~0.34x realtime); `tiny.en` = 1.7s wall time (~0.16x realtime, roughly 2x faster), with only a trivial punctuation difference in output (missing one comma) — no meaningful accuracy loss on this sample. Kept `base.en` as the shipped default in `configs/example.conf` for its better accuracy margin on harder audio (accents/noise/vocabulary), since even its latency is comfortably within "near real-time" for short PTT clips; `tiny.en` is documented as the lower-latency alternative for anyone who wants to trade accuracy for speed. Note: the real end-to-end daemon run (`n_threads=8`, see above) took roughly 1x realtime on a 3s clip (~3s from capture-stop to inject) — somewhat slower proportionally than this isolated 4-thread benchmark, plausibly first-inference warmup or thread-count contention on an 8-thread/4-core CPU; not investigated further since it's still within acceptable PTT latency.
