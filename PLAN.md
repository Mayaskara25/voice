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

### Post-Phase-D — the clip truncation, fixed in one shared place (done)

- **`src/xclip.{c,h}`**: one `xclip_clamp()` used by both rasterizers. The narrowing to
  `XRectangle`'s 16-bit fields lives *inside* the function, because the narrowing is the bug — a
  helper that only clamped and left the cast at the call site would not test the thing that
  breaks. Plain `int` in, so the file needs nothing from microui and can go in both `SRCS` and
  `SETUP_SRCS` without disturbing the setup binary's `-lX11`-only link.
- **It was reachable in the status panel, which the earlier "latent" note understated.**
  `MU_COMMAND_CLIP` is only emitted when text is *partially* clipped (`microui.c:493/503`), and
  vertical rows tile exactly so they rarely straddle — but a **word wider than the panel** always
  does, and a transcript can easily contain one (a URL, a long filename). With such a token the
  sentinel is emitted on an ordinary frame.
- **Proved by A/B, same probe and geometry, only the clamp differing.** A temporary probe logged
  the translated rectangle and put a label after `mu_text`:

  | clamp | `in=16777216x16777216` becomes | `PROBE-FOOTER` |
  |---|---|---|
  | raw cast | `0x0` | missing |
  | `xclip_clamp` | `300x100` | visible |

  Two earlier attempts at a visual repro were **wrong and discarded**: with the text row laid out
  as `-1` (extend to bottom) the footer lands below the 100px panel and is invisible for a
  mundane layout reason, which is indistinguishable from the bug. That is why the probe ended up
  logging the actual translated rectangle rather than relying on what did or did not appear.
- `tests/test_xclip.c` pins the sentinel case plus negative origin, negative extent (must not
  wrap to 65531), oversize, fully-off-surface and degenerate-surface. Confirmed to have teeth by
  restoring the raw cast and watching 6 assertions fail.
- Regression: the setup window renders unchanged with the shared helper (its inline copy was
  removed), and `./dictation --gui` shows the panel with `_NET_ACTIVE_WINDOW` identical before
  and after — a meaningful check this time, since a window actually held focus.
- The probe was removed before committing and `git diff` reviewed to confirm only the intended
  change remained.

### Phase D follow-up — removing models, and a Makefile bug worth more than the feature (done)

- **`[Remove]` sits where `[Get]` sits.** The row already had a fourth column that was an empty
  label for present entries, so the action column now simply follows the state: `[Get]` when
  missing, `[Remove]` when present. No layout change, and `model_row()`'s existing `mu_push_id`
  already prevents the per-row id collisions.
- **Deleting is confirmed, and the dialog names every path it will unlink.** That disclosure is
  the safety mechanism, not a nicety: for a symlinked model the file actually removed is *not*
  the path shown in the model list. The dialog reuses the download prompt's container with a
  `prompt_kind`, rather than adding a second popup.
- **The symlink policy, which is the whole design question.** `models/*.bin` here are commonly
  symlinks into `whisper.cpp/models/` — that is what the README's own `ln -sf` step creates, and
  on this machine 2.1 GB of models live behind them. Unlinking only the link would free **zero
  bytes** while the row flipped to "missing": a display that lies about the disk. So the target
  is deleted too, but **only when it resolves inside the project**; a target outside is left
  alone and the dialog says "keeps" next to it and reports 0 MB freed. Checked first that these
  targets are untracked and gitignored (`whisper.cpp/models/.gitignore:1`), so deleting them
  cannot dirty the tree.
- **The containment test is where the bug would have been.** A plain `strncmp` prefix check says
  `/home/u/voice-backup` is inside `/home/u/voice` — in a function that authorises deletion.
  `setup_path_within()` requires the next byte to be `/` or the strings to be equal, and both
  sides are `realpath`'d first (comparing a resolved target against an unresolved anchor gives
  false negatives when the project is reached through a symlinked path). The anchor is resolved
  once at init; if it cannot be resolved it is left empty, which makes every target read as
  *outside* — the safe direction to fail.
- **Deleting the selected model warns about Super+D specifically.** The window's own Start gate
  catches a config naming a deleted file, but the keybind does not: it goes straight to the
  script, to the daemon, to `config_validate`'s fatal, and the user gets a dictation key that
  silently stopped working. The dialog says so before the delete, not after. The selection is
  deliberately left pointing at the deleted entry, which now reads "missing", so Start offers to
  fetch it back.
- **`setup_path_within`, `setup_plan_removal` and `setup_apply_removal` are exported and unit
  tested** (`tests/test_setup.c`, no Display needed) across plain file, symlink-inside,
  symlink-outside, dangling symlink, directory, missing file and empty path — the cases a click
  through the window never reaches. The prefix-boundary assertions were confirmed to have teeth
  by replacing the check with `return 1` and watching exactly those two assertions fail.
- **The Makefile had no header dependencies, and it corrupted memory.** `%.o: %.c` listed no
  `.h` prerequisites, so editing a header rebuilt nothing. Adding a field to `struct setup_gui`
  left `setup_main.o` allocating the *old, smaller* struct on the stack while the freshly built
  `setup_gui.o` did `memset(g, 0, sizeof(*g))` at the *new, larger* size — writing past the end
  of the caller's frame and zeroing `main`'s `project_dir`, which surfaced as the launcher
  script suddenly being looked for at `/scripts/waybar-dictation.sh`. That is a silent
  ODR-style mismatch that any future header edit could reproduce anywhere in the project, so it
  is fixed properly with `-MMD -MP` and `-include $(DEPS)`, `.d` files added to `clean` and
  `.gitignore`, and the whole tree rebuilt from clean so no stale object survives. Verified:
  touching `setup_gui.h` now rebuilds `setup_main.o` as well, where before it rebuilt nothing.
- **The id-stack trap caught me a second time, from the other direction.** `model_row()` wraps
  each row in `mu_push_id`, so `mu_open_popup(SETUP_PROMPT)` called from `[Remove]` hashed to a
  different container than the one drawn at window depth, and the dialog silently never
  appeared. (The Start button had worked only because it happens to sit at window depth.)
  Rather than patch the call site, opening is now deferred through a `prompt_pending` flag and
  performed in exactly one place in `render()`, immediately above where the dialog is drawn, so
  the two cannot drift apart again.
- The dialog is 560px wide and taller for the delete case, because an absolute model path was
  being clipped at the old 380px — a clipped path defeats the disclosure the confirmation
  exists to provide.
- `[Remove]` is inert while a download runs, mirroring `[Get]`: curl renaming its `.part` onto a
  path being unlinked concurrently is a race with no good outcome.
- **Verified live against a scratch `--models-dir`, never the real models** (a mistaken delete
  there costs a 1.6 GB re-download and there is no undo): plain file frees its own size;
  in-project symlink removes both link and target and frees the target's size; out-of-project
  symlink removes the link, **leaves the target on disk** (checked directly) and honestly
  reports 0 MB freed; Cancel deletes nothing; rows flip to "missing" with `[Get]` afterwards.
  `configs/local.conf` was backed up and restored, and the real `models/` directory is
  byte-for-byte as it was.
- **Paths in the delete dialog are elided to fit, measured against the real font.** `mu_text`
  only breaks a line at a space and its inner loop refuses to break when the word is the first
  on the line (`microui.c:714-716`), so a path — which has no spaces — is never wrapped: it is
  drawn at full width and clipped by the X clip rect, with nothing on screen indicating it
  happened. That silently truncates the one thing the confirmation exists to disclose. The
  earlier "widen the window" response was not a fix: the project's own resolved target is 78
  characters, which is 468px in the 6px `fixed` fallback (fits) but 702px in the `9x15` this
  window actually asks for (clips) — so the defect was latent purely because `9x15` is not
  installed on this machine, and installing `xorg-fonts-misc` would expose it. `fit_path()` now
  elides the middle, measuring with `ctx->text_width` rather than assuming a character width,
  and grows the tail first so the filename — the part that says what is about to be deleted —
  always survives. Verified with a 143-character path, which renders as
  `...bbbb/cccc/dddd/eeee/ggml-base.en.bin`. The untruncated paths are written to both the log
  pane and stderr, since the pane wraps on spaces too.
- The message buffer went from 640 to 1024. With `link` and `target` each `CONFIG_PATH_MAX`
  (512), the two path lines alone could reach 1024 bytes and `msg_append` would park at the NUL
  — not an overflow, but the *tail* is dropped, and the tail is the "frees N MB" line and the
  "Super+D will fail" warning. Same failure class as the clipping above by a different
  mechanism: the confirmation silently dropping the information that makes it safe. Elision
  already bounds each path to the body width, so this is belt-and-braces.
- **A reviewer's claim that `MU_OPT_POPUP` blocks clicks reaching controls behind the dialog is
  wrong, and the code comment now says why.** A static read of `in_hover_root()` supports it,
  but `hover_root` is resolved from the *previous* frame's `next_hover_root`, and moving the
  pointer onto that control — which a real mouse necessarily does — has already switched
  `hover_root` back to the main window before the press lands. Verified live: with the delete
  dialog open, clicking a catalog row both dismissed the dialog and selected that row, writing
  `configs/local.conf`. The `!dl_active` guards are therefore genuinely reachable, not dead.
- **Deleting the link is now conditional on the target actually going**, which is a real fix
  rather than a tidy-up. The code unlinked the link unconditionally while its own comment
  claimed otherwise, so a failed target unlink (permissions, read-only mount) would have
  removed the only name pointing at the file: the bytes would still be on disk, no row would
  reference them, the entry would read "missing", and re-downloading would allocate the space a
  *second* time — a silent disk leak that compounds per cycle. Keeping the link on failure
  leaves the model working and visible so the error can be seen and retried. Covered by a test
  that forces the failure with an unwritable target directory, and confirmed to have teeth by
  restoring the old behaviour and watching exactly those assertions fail.
- **Measured, not reasoned about: repeated download/delete cycles do not accumulate.** A
  fixture reproducing the production layout (a *relative* symlink into a vendored directory,
  plus a `file://` catalog entry so no network is involved) was cycled three times through
  remove → re-download using the real `setup_apply_removal` and the real `--fetch-model`. Disk
  usage returned to the identical byte count after every removal (20,971,630) and after every
  download (41,943,150), with no orphans left anywhere in the tree. Worth noting what the first
  cycle changes: the symlink is replaced by a **regular file** under `models/`, so from then on
  the plan reports "removes the file" with no target, and the storage moves out of the vendored
  directory permanently.
- **A third instance of the same bug class, caught in review before shipping**: the delete
  dialog built its text with `n += snprintf(msg + n, sizeof(msg) - n, ...)`. `snprintf` returns
  what it *would* have written, so once anything truncates, `n` passes the buffer end, the
  `size_t` subtraction underflows to a huge value, and the next call writes off the frame. It
  concatenates a 96-byte display name and two `CONFIG_PATH_MAX` paths against a 640-byte buffer,
  so a long enough `--models-dir` reaches it. Replaced with a clamping `msg_append()` helper.
- **The production symlink form is relative, and the first round of tests only used absolute
  targets.** `models/x.bin -> ../whisper.cpp/models/x.bin` is what `ln -sf` creates and what is
  on disk. A relative-symlink fixture is now in `test_plan_removal`, asserting the target still
  resolves as inside the project, still reports the target's size, and that the stored path is
  fully resolved rather than containing `..`. Two couplings it pins: `realpath` resolves a
  relative link against the *link's* directory, and resolves the relative `./models/...` catalog
  path against the CWD, which only works because of the startup `chdir`.
- **The daemon binary was rebuilt by `make clean` and had never been run**, so it was smoke
  tested through the real launcher script rather than assumed good: `waybar-dictation.sh start`
  → both models loaded → `dictation: ready` → `status` reports `active` → `stop` → `notactive`.
  Left stopped.
- Pre-existing warning surfaced by the first full rebuild in a while, left alone because it is
  in the daemon and unrelated to this work: `src/hotkey_evdev.c:166` `-Wformat-truncation` on a
  `%s` into a 300-byte buffer. Worth its own commit.

### Phase D follow-up — more models, and a download prompt on Start (done)

- **Four catalog entries added**, all sizes taken from the real `content-length` rather than
  estimated: whisper `large-v3-turbo` (unquantized, 1624555275) and `medium.en` (1533774781);
  llama `gemma-3n-e2b-q3km` (2483178848) and `gemma-3n-e2b-q4km` (3026881888). The
  `large-v3-turbo` figure was independently confirmed against the copy already on disk — the
  local file is exactly 1624555275 bytes, matching the header — which is a free check that the
  URL and the entry really refer to the same artifact.
- **"gemma4 e2b" is Gemma 3n E2B.** There is no Gemma 4; E2B is Gemma 3n's MatFormer
  designation for ~2B *effective* parameters out of ~5B raw, which is why a "2B" model is a
  2.3–2.8 GB file rather than ~1 GB. Checked before adding it that the pinned llama.cpp can
  actually load one: `LLM_ARCH_GEMMA3N` is in `llama-arch.cpp` and `src/models/gemma3n.cpp`
  exists. A catalog entry for an architecture the vendored runtime cannot load would be a
  multi-GB download ending in a load failure.
- **VRAM is the real constraint, so it is in the display names.** Whisper runs on the CPU
  backend here, but cleanup models go on the GPU at `n_gpu_layers=99`, and this machine's GTX
  1650 has 4096 MiB. Q4_K_M (2.8 GB) plus the ~300 MB compute buffer and KV cache is tight;
  Q3_K_M (2.3 GB) is comfortable. **Both** are listed rather than one being chosen, since the
  catalog is data and picking should be a click, not an edit. The sizes and the "tight on 4 GB
  VRAM" note live in the `display` field because that is the only place the user sees them
  before committing to the download.
- **No dropdown.** microui has no combobox, and building one out of `mu_open_popup` would be
  the most code for the least benefit at seven rows. The existing flat radio rows were kept —
  `model_row()` already handles the id collisions correctly via `mu_push_id`. If either list
  passes roughly 8–10 entries, wrap that section in `mu_begin_panel` with a fixed height (three
  lines); deferred until it is real rather than done speculatively.
- **Start now prompts instead of being inert.** This is a deliberate deviation from the
  locked-in "Start is gated" decision above, and the gate's *purpose* is preserved exactly: a
  click never starts a daemon that would hit `config_validate`'s fatal. Start becomes clickable
  when a selected model is merely *missing*, and that click opens a centred confirm dialog
  ("… is not downloaded yet … Download it now (N MB)?") whose confirm button reuses the same
  `start_download()` the `[Get]` buttons call. It stays inert with nothing selected, no launcher
  script, or a download already running. It deliberately does **not** chain into starting the
  daemon when the download finishes — silently launching after a multi-GB fetch is surprising,
  so the user presses Start again.
- **A real pre-existing hole was found and closed while doing it**: the old gate checked only
  the whisper model, but `config_validate` (`src/config.c:148-152`) treats a `llama_model_path`
  naming a missing file as fatal. So selecting an undownloaded *cleanup* model and pressing
  Start produced precisely the error Phase D exists to eliminate. `first_missing_required()`
  now checks both, whisper first. A blank llama path (the "none (raw whisper)" row) still never
  gates Start. Verified live: with a present whisper model and a missing Gemma selected, Start
  opens the prompt naming the Gemma rather than starting the daemon.
- **microui gotcha worth recording, because it cost a debugging round and would recur.**
  `mu_begin_window_ex` pushes the window's id onto the id stack (`microui.c:1088`) and
  `mu_get_id` seeds its hash from the top of that stack (`microui.c:232`). So the *same* popup
  name resolves to **different containers** depending on whether the call sits inside or outside
  the enclosing `mu_begin_window`/`mu_end_window` pair. The first version opened the popup from
  inside the window and drew it from outside: `mu_open_popup` set `hover_root` on container A
  while container B was drawn, so `MU_OPT_POPUP`'s "close when a press lands elsewhere" test
  compared against the wrong container and closed the dialog on the very frame it opened —
  presenting as a Start button that simply did nothing. Both calls must sit at the same
  id-stack depth; microui's own demo does exactly this. Being nested does not clip the popup:
  `begin_root_container` pushes `unclipped_rect`.
- `mu_get_container()` **creates** a container with `open = 1`, so popup visibility is tracked
  in `prompt_index` rather than read back from microui — merely looking the container up in
  order to ask "is it open?" would make it appear. For the same reason `prompt_index` is set to
  -1 explicitly in `setup_gui_init`: the `memset` there leaves 0, which is a valid catalog
  index, and the dialog would have been up the moment the window opened.
- **A second bug, found by asking what happens if a download is already running.** The dialog
  does *not* reliably block clicks on the controls behind it — observed directly: a click on a
  model row while the dialog was open selected that row. So `[Get]` can be pressed with the
  dialog still up, leaving a download running underneath it, and the dialog's confirm button had
  no `!dl_active` guard where `[Get]` (`setup_gui.c:453`) has one. Pressing it would have called
  `start_download` over a live `struct download`, orphaning the running curl child and leaking
  its pipe fd. The confirm button is now inert and relabelled "Download now (busy)" while a
  download runs, mirroring `[Get]`.
- The `why` line gained a `dl_active` case. Start is disabled during a download, and without it
  the button was simply dead while the line underneath explained something else entirely; it now
  reads "downloading; Start waits until it finishes".
- **The headless path was re-checked with the new ids**, since the README presents it as the
  no-X fallback: `--fetch-model large-v3-turbo` reports "already at
  ./models/ggml-large-v3-turbo.bin" rather than resolving to the `large-v3-turbo-q5_0` entry it
  is a strict prefix of — `catalog_find` was read as exact `strcmp`, but this is the check that
  the *chosen ids* behave. `--fetch-model gemma-3n-e2b-q3km` fetched from the right URL with the
  catalog's expected size, and SIGINT left no `.part` and no orphaned curl. An unknown id
  (`gemma4-e2b`, the name this all started from) is still rejected with a pointer to `--list`.
- **Test-harness note, not a user-facing bug**: microui resolves `hover_root` one frame behind,
  so a synthesized single move+press onto a *newly appeared* popup is ignored — the press is
  evaluated while `hover_root` is still the main window. The XSendEvent driver therefore needs a
  separate hover frame before the click. A real pointer generates motion events continuously on
  its way to the button, and the dialog is centred while Start is bottom-left, so the cursor is
  never already over it when it appears.
- Verified live end to end with screenshots at each step: all seven rows render with correct
  present/missing status and the size/VRAM hints fit the name column; no dialog on startup;
  Start on a missing whisper model opens the prompt; Cancel dismisses it; Start on a missing
  cleanup model names the cleanup model; **Download now** launched curl with the correct
  `--fail`/`.part` command line, streamed its meter into the pane and drove the progress bar.
  The download was then cancelled, the `.part` file was removed by the existing cancel path, and
  `configs/local.conf` was restored to the models it named beforehand. `make test` green across
  all ten suites.

### Phase D5 — desktop integration (done)

Phase D's sub-phases D0–D5 are all complete. Of the four Done-when criteria at the top of the
Phase D section, three are verified below and the fourth — a real PTT dictation into a focused
editor with `./dictation --gui`, confirming the editor never loses focus — needs a physical
keypress and remains the user's to make. It is the same boundary D4 drew around Super+D (since
confirmed by the user). Do not read "D5 done" as "Phase D signed off".

- **`dictation.desktop.in` + `make install-desktop` / `uninstall-desktop`.** Tracked as a
  template because `Exec=` must be an absolute path and that path is machine-specific;
  `install-desktop` seds `@BINDIR@` → `$(CURDIR)` into
  `$(XDG_DATA_HOME)/applications/dictation.desktop`. Per-user, so no root and no system
  directories touched. Nothing generated lands in the tree, so `clean` and `.gitignore` needed
  no changes.
- **`install-desktop` depends on `$(SETUP_BIN)`, deliberately not on `all`.** `all` pulls
  `deps-whisper deps-llama`, which would have dragged the ~10-minute CUDA build into the one
  target whose entire point is being usable before it — silently destroying the D0 payoff. The
  dependency also means the installed entry can never point at a binary that was never built.
- The sed delimiter is `|`, not `/`, because the replacement is a path. Destination is
  `mkdir -p`'d first (not guaranteed to exist on a minimal system), and
  `update-desktop-database` is guarded by `command -v` — a genuine no-op with no `MimeType=`,
  but free and expected by desktop tooling.
- **`Exec=` points into the build tree (`$(CURDIR)`), which is correct here**, not a shortcut:
  there is no `make install` for the binaries, `models/` and `configs/` live in the checkout, and
  `resolve_project_dir()` locates the project relative to the executable. An entry pointing
  anywhere else would break that chdir. Documented in the README that moving the repo means
  re-running `make install-desktop`.
- **`desktop-file-validate` exits 0** with one hint:
  `Categories=Utility;Accessibility;AudioVideo;` "contains more than one main category;
  application might appear more than once in the application menu". Left as-is — it is a hint,
  not an error, and only one entry actually appeared. Trim the list if duplicates ever show up
  in a real menu.
- **The launcher path was the only thing here that could genuinely be wrong**, and it is the
  first real exercise of D4's `/proc/self/exe` chdir. Checked twice: (a) `cd $HOME &&
  /home/.../dictation-setup --list` with the environment preserved — logged `setup: working
  directory /home/mayaskara/projects/voice` and resolved all three catalog entries; (b) the
  installed entry launched for real with `gio launch`, which rendered both model lists with
  correct `ready` status, the pre-selected models from the config, and Start enabled.
  Deliberately not `env -i`, which would have stripped `DISPLAY`/`XAUTHORITY` and failed for an
  unrelated reason that proved nothing.
- **`StartupWMClass=Dictation` confirmed against the live window**, not just against the source:
  `xprop` on the launched window gives `WM_CLASS = "dictation-setup", "Dictation"`, matching the
  `XSetClassHint` in `setup_gui.c`. `_NET_CLIENT_LIST` held exactly one window, so the entry does
  not produce a second unnamed dock icon.
- **Regression re-checked with the window open**: `pgrep -x dictation` printed nothing — the
  process-identity collision that killed the `--setup` design, asserted directly rather than
  assumed. The user separately confirmed **Super+D** still behaves exactly as before, closing the
  one D4 item that needed a physical keypress. The daemon was left stopped and the tree clean.
- **README brought up to date for the whole of Phase D**, which it had not been since D1: Status
  row D marked done, the setup window documented as the recommended path with `--list` /
  `--fetch-model` kept as a documented headless fallback (a fresh clone over ssh still needs it),
  the manual `download-ggml-model.sh` flow kept below that rather than deleted, and the intro's
  "the whole thing is a single C program" corrected — it is now a daemon plus a companion binary,
  and the sentence that matters ("the daemon contains no network code") is stated explicitly
  instead of being implied by the old wording.
- Two README sentences exist to stop a reader drawing a wrong conclusion. First, that the Wayland
  limitation applies **only to keystroke injection**, not to the windows — both GUIs are ordinary
  X11 clients and render through XWayland anywhere, so the new section does not read as
  contradicting the `ydotool` section. Second, that `Terminal=false` sends startup errors (no X
  display, unreadable catalog) somewhere a launcher gives the user nowhere to look, so a dead
  icon is diagnosable via `journalctl --user`. Documentation only; a fallback error dialog was
  considered and rejected as out of scope.
- **Still deliberately not done**, carried from D4: tailing the daemon's `$LOG_FILE` into the
  output pane for diagnosing a failed start. Phase D's contract is the readiness state, which
  comes from the script. It remains the most useful next increment if the window gets more work.
- **Still outstanding and untouched by design**: the `unsigned short` clip truncation at
  `src/gui_xlib.c:151-157`, the exact bug fixed in `setup_gui.c` during D3. It is latent in the
  status panel (it needs both a partially-clipped string and something drawn after it), and
  Phase D's contract is that `gui_xlib.c` is not modified, so fixing it belongs in its own
  deliberate commit against a daemon that is in daily use.
- **`Icon=audio-input-microphone` confirmed to actually resolve**, not just assumed to be a stock
  name: 64 matching files across the installed themes (breeze-dark, Adwaita, …). A stock name that
  no installed theme provides would have rendered as a blank tile in the launcher, which is
  invisible in every check that does not involve a real menu.
- **`./dictation --gui` re-checked directly** (the fourth Done-when criterion, and the one Phase D
  could most plausibly have broken, since `SRCS` *did* change across D0/D1 — `llm_styles.c` split
  out, `config_write.c` and `model_catalog.c` added, and `main.c:241` now calls
  `config_default_path()`). The daemon started, logged `gui: status panel created (300x100,
  override-redirect)`, reached `dictation: ready`, and shut down cleanly on SIGINT.
  `_NET_ACTIVE_WINDOW` was `0x0` both before and after the panel appeared, so no focus was taken —
  **but note that check is weak here**: nothing else on this X server held focus to begin with, so
  "unchanged" is close to vacuous. The mechanism (override_redirect=True) is what actually
  guarantees it, and the real test is the user's dictation-into-an-editor pass.
- Incidental finding, recorded because it is confusing otherwise: under Hyprland's XWayland the
  panel **does** appear in `_NET_CLIENT_LIST` despite being override-redirect (it showed up as
  `0x600002` with no `WM_CLASS`/`WM_NAME`, and disappeared exactly when the daemon stopped, which
  is how it was identified). That is a compositor quirk, not a regression — presence in that list
  is not the same as being focusable, and focus was not taken.
- Also incidental, D3 territory rather than D5: the setup window's default font `9x15` is **not
  available on this X server** and `font_open` falls back to `fixed` (visible in the launch log;
  the daemon's panel asks for `fixed` directly and gets it, so only the setup window is affected).
  The spec's rationale for 9x15 was that 13px is cramped for a widget-dense form, so that intent
  is currently unrealized here. It renders perfectly legibly regardless — see the D5 screenshot —
  so this is a note, not a defect. `--font XLFD` overrides it. (`xlsfonts` is not installed on
  this machine, so the fallback log line is the evidence, not a font-list dump.)
- `make test` runs all ten suites green with no skips (`test_stt` included, since the sample wav
  and models are present). No file in `SRCS` changed **in this phase**, so the `dictation` binary
  is byte-identical to the one D4 left behind — though not to the pre-Phase-D one, per the D0/D1
  changes noted above.

### Phase D4 — daemon start/stop from the window (done)

- **Start/Stop shell out to `scripts/waybar-dictation.sh`**, never reimplemented. That script
  already owns the mandatory `cd "$DICT_DIR"` (without which every relative model path breaks),
  the `nohup` detach, the log file and the Waybar refresh signal; a second launcher in C could
  disagree with it about process identity, which is by executable name (`pgrep -x dictation`).
  Because this binary is named `dictation-setup`, that `pgrep` never matches it and the script
  needed no identity changes at all.
- **State comes from the script's own `status` JSON**, substring-matched on the fully-quoted
  `"class":"active"` / `"loading"` / `"notactive"` forms. Note `"notactive"` contains `active`,
  but `"class":"active"` is not a substring of `"class":"notactive"` — the quote disambiguates,
  which is why the quotes are part of the pattern rather than incidental.
- **Nothing blocks the render loop.** `status` is a `pgrep` plus a `grep`, so it is captured
  synchronously through a pipe; `start`/`stop` are **double-forked** so the grandchild is
  reparented to init and never waited on — the script's `stop` polls for up to 5 s for the daemon
  to exit, which would otherwise freeze the window solid. The intermediate child is reaped with
  `waitpid(WNOHANG)` on the tick, not in the click handler, and always by explicit pid so it can
  never steal curl's child out from under `download_reap`.
- **Poll cadence**: 1 s normally, 300 ms while `loading` so the transition is visible. The window
  therefore stops being purely event-driven, which is deliberate: the daemon's state changes
  behind its back whenever the user presses **Super+D**, and a status line that only updates on
  mouse movement would be wrong more often than right. Verified: starting the daemon from a
  terminal while the window sat idle flipped it to "Stop dictation / daemon: ready" on its own.
- **The window now chdir's to the project directory at startup** (resolved from `/proc/self/exe`,
  and *only* if that directory actually contains `scripts/waybar-dictation.sh`; `--dir PATH`
  overrides). This is D5 insurance as much as D4 plumbing: a `.desktop` launcher starts a process
  with `cwd=$HOME`, which would have put `configs/local.conf` in the user's home directory and
  found no catalog at all. A copy installed in `/usr/local/bin` fails the probe and leaves cwd
  alone rather than doing something worse. (The probe is exactly "is there a `scripts/` next to
  the resolved executable" — nothing more general. A symlink from `~/bin` into the repo resolves
  to the repo and chdirs there, which is the wanted outcome, but this does not detect installed
  copies in the abstract.) Verified by running `--list` from `/tmp`.
- **`scripts/waybar-dictation.sh`: `DICT_CONF` now prefers `configs/local.conf` when it exists** —
  the follow-up carried since D1, and the one edit in this phase that could break a daily driver.
  All four states were checked with `bash -x` rather than by reading the diff: absent → resolves
  to `example.conf` (today's behaviour, unchanged), present → resolves to `local.conf`, `bash -n`
  clean, and `status` prints byte-identical JSON. Written as an explicit `if` block rather than
  `[ -f … ] && DICT_CONF=…`, which under `set -uo pipefail` would return nonzero if it ever ended
  a function. A daemon started from the script and a bare `./dictation` now always agree on which
  config they read.
- **Stop is never gated**; Start keeps its gate on the selected whisper model being present, and
  both are disabled with the reason shown if the launcher script isn't found. That disabled path
  was rendered and clicked, not just reasoned about — `--dir` pointed at a directory with no
  `scripts/`, the row read "scripts/waybar-dictation.sh not found -- Start/Stop unavailable", and
  clicking the button started nothing. It is the path a `.desktop`-launched installed copy hits,
  so D5 will meet it first.
- **Verified live**, real script and real daemon: pressing **Start** in the window walked
  `daemon: not running → loading models... → ready` (screenshots at each step), `pgrep -x
  dictation` showed the daemon, the button label swapped to **Stop dictation**, and the exact
  command run was echoed into the log pane. **Stop** ended it within ~1.5 s and the label swapped
  back. No zombies under the setup process, and the daemon was left **stopped**, as it was found.
- Not verified here (needs a physical keypress): pressing **Super+D** while the setup window is
  open. The mechanism is the same one exercised above — the keybind calls the same script, and the
  window merely polls its `status` — but the keypress itself is the user's to make.
- **Deliberately not done**: tailing the daemon's `$LOG_FILE` into the output pane. It would be
  genuinely useful for diagnosing a failed start, but D4's contract is the readiness state, and
  that comes from the script. Worth considering alongside D5.
- `make test` still runs all ten suites green, and no file in `SRCS` changed, so the `dictation`
  binary is untouched by this phase.

### Phase D3 — the setup window (done)

- **`src/setup_gui.{h,c}`**: WM-managed microui/Xlib window, ~700 lines, a sibling of
  `gui_xlib.c` rather than an edit of it. `gui_xlib.c` is untouched, so the focus-preserving
  status panel cannot regress. `dictation-setup` with no arguments now opens the window; `--list`
  and `--fetch-model` stay as the headless D2 equivalents (still useful over ssh), and a missing
  `$DISPLAY` says so and points at them instead of aborting inside Xlib.
- **Inversions from the panel, all as planned**: `override_redirect` False (verified WM-managed —
  the window appears in `_NET_CLIENT_LIST`), `WM_DELETE_WINDOW` handled, `KeyPressMask` for
  Esc-only, `XStoreName` + `XSetClassHint`. Confirmed with `xprop`:
  `WM_CLASS = "dictation-setup", "Dictation"` — the `res_class` D5's `StartupWMClass` must match.
- **Three bugs found by actually running it**, none of which a unit test would have caught:
  1. **The microui container never followed the X window.** `mu_begin_window_ex` applies its rect
     only on first use (`if (cnt->rect.w == 0)` in microui.c), so the content stayed 560x640
     forever. This is not a corner case: Hyprland is tiling and resized the window to 926x472 on
     map, leaving the content in a corner with the whole footer cut off. Fixed by re-asserting
     `mu_get_container(ctx, SETUP_TITLE)->rect` every frame.
  2. **`0x1000000` truncates to 0 in an `XRectangle`.** microui's "no clip" sentinel is
     `{0, 0, 0x1000000, 0x1000000}` (microui.c:50), and the rasterizer copied from `gui_xlib.c`
     narrows `w`/`h` into `unsigned short` — giving a **0x0 clip that silently discards every
     later draw**. `mu_draw_text` emits that sentinel to restore the clip after any
     partially-clipped string, so with a scrolling log pane it fired on nearly every frame: the
     curl output and the entire footer vanished. Found by dumping the command stream after the
     symptom survived a forced redraw. Fixed by clamping the clip to the window before narrowing.
     **This same latent bug is still in `src/gui_xlib.c:151-157`** — see the follow-up below.
  3. `font_open` logged the *requested* font name after falling back, which makes a layout
     rendering at the wrong size look inexplicable. Now logs what actually loaded. (Log text
     only — the daemon's behaviour is unchanged.)
- **Traps handled up front, per the plan**: every row is wrapped in `mu_push_id` (several buttons
  labelled "Get" in one container would otherwise collide and route clicks to the wrong row); the
  `(*)`/`( )` marker is its **own column**, not part of the button label, because `mu_button`
  hashes the label and a label flipping `( )`→`(*)` would change the control's id between frames;
  the log pane auto-scrolls by overshooting `scroll.y` and letting microui clamp;
  `MU_COMMAND_ICON` stays a no-op and the layout deliberately uses no widget that emits one.
- **Log ring is 64 KB stored but only the last ~8 KB rendered**, aligned to a line boundary.
  `mu_text` emits one text command per wrapped line and `mu_push_command` *aborts the process* if
  the 256 KB command list overflows, so rendering a full 64 KB of curl meter output is not safe.
- **Start is gated** on `sel_whisper >= 0 && present`, with the reason on screen ("select a
  whisper model and Get it first"). It is rendered but not yet wired: pressing it prints the
  literal `scripts/waybar-dictation.sh start` command it will run in **D4**.
- **Selection is pre-loaded from the config**, not defaulted to the first row, so the window opens
  on what the daemon would actually read. When a configured path isn't a catalog entry, the pane
  says so explicitly for *both* model kinds — otherwise the list would show "(*) none (raw
  whisper)" while the config names a cleanup model, i.e. the display would be lying.
- **The D2 post-reap drain is carried into the GUI loop**, for the same reason: on a fast failure
  the text still buffered in the pipe when `waitpid` succeeds *is* the error message.
  `download_cancel` runs from `setup_gui_destroy`, so Esc, the WM close button and SIGINT all
  converge on one place that guarantees no orphan curl.
- **Verified live** (X11 via XWayland under Hyprland; clicks delivered with `XSendEvent`, which
  targets the window directly and so neither moves the user's pointer nor depends on where the
  compositor placed the window; screenshots via `import`):
  - **Regression check #1 from the plan: `pgrep -x dictation` prints nothing while the setup
    window is open.** This is the collision that killed the `--setup` design; asserted directly,
    twice.
  - clicking a model name moves the marker and writes `configs/local.conf` (comments intact,
    both keys correct); "none (raw whisper)" writes an empty `llama_model_path`.
  - clicking **Get** on a missing model runs curl in-window: the command echoes as the first log
    line, the meter renders as readable separate lines (CR translation working), the bar reaches
    `100%  2 / 2 MB`, the row flips MISSING→ready, its Get button disappears, and — because no
    whisper model had been chosen — it auto-selects and writes the config. Note this used a
    **`file://` catalog, not a HuggingFace URL**: every model in the real catalog is already
    present on this machine, so the real-network path has only ever been exercised headlessly
    (D2's 148 MB `base.en` fetch). It is the same `downloader.c` either way.
  - Esc and the WM close button both exit cleanly with no Xlib fatal error.
  - closing **with a download in flight** cancels it: window exits, no orphan curl, no `.part`.
  - `./dictation --gui` still brings up the override-redirect status panel unchanged.
- **`9x15` is not installed on this machine**, so the window runs on `fixed` (13px). It is
  readable but is the cramped case the plan chose 9x15 to avoid; on Arch the font comes from
  `xorg-fonts-misc`. `--font XLFD` overrides it.
- **Observed once, not reproduced**: during bring-up a single Get click produced hover but no
  download. Four subsequent clean trials (three scripted, one instrumented showing
  `MU_RES_SUBMIT`) all worked, and no mechanism was found. Recorded rather than dropped, in case
  it recurs.
- **Build**: `src/setup_gui.c` joins `SETUP_ONLY_SRCS` (so tests can link it) and `SETUP_SRCS`;
  `SETUP_LDLIBS` becomes `-lX11`. `font_xlib.c`/`microui.c` are deliberately *not* in
  `SETUP_ONLY_SRCS` — they are already in `SRCS`, and the same object twice on a link line is a
  duplicate-symbol error. `dictation-setup` is **79 KB** and `ldd` shows only libX11 + libc:
  still no whisper, llama or CUDA, and still no `deps-*` prerequisite.
- **`tests/test_setup.c`** covers what needs no display: the ring's whole-line eviction (including
  a single write larger than the ring, and a newline-free ring), the tail view's line alignment,
  and the selection write including the empty-`llama_model_path` case. `make test` runs all ten
  suites green.
- **Follow-up worth doing, deliberately not done here**: `src/gui_xlib.c:151-157` has the same
  `unsigned short` clip truncation. Two conditions currently keep it latent, and **both** matter:
  the panel needs a *partially clipped* string to make `mu_draw_text` emit the sentinel at all
  (in a 300x100 panel that means a transcript long enough to overflow its text area), **and**
  `mu_text` is the last thing that panel draws, so the truncated clip dies with the frame. Adding
  any widget after the transcript reintroduces the bug even if the text is usually short. The fix
  is the same ten lines, but the daemon is in daily use on this branch and Phase D's contract is
  that `gui_xlib.c` is not touched, so it should be a separate, deliberate commit.

### Phase D2 — downloader + headless `dictation-setup` (done)

- **`src/downloader.{h,c}`**: `download_check_curl` / `download_start` / `download_read` /
  `download_update_progress` / `download_reap` / `download_cancel`, plus a pure
  `download_translate_cr()`. `pipe2(O_CLOEXEC)` + `fork` + `execvp("curl", …)` with the child's
  stdout *and* stderr dup2'd onto the one pipe, parent's read end `O_NONBLOCK`. Command is
  `curl -L --fail -o <dest>.part <url>`, kept in `d->cmdline` so the log pane's first line is a
  copy-pasteable reproduction of the failure.
- **`src/setup_main.c`**: `dictation-setup [--catalog PATH] [--models-dir DIR] [--list]
  [--fetch-model ID]`. No GUI yet — D3 puts a window on exactly these calls. `--list` prints the
  catalog with ready/MISSING status; `--fetch-model` runs the poll/read/tick/reap loop headless.
- **The payoff of D0, measured**: `dictation-setup` is **40,912 bytes** against the daemon's
  **73,334,824**, and `ldd` shows *only* libc — no X11, no CUDA, no llama/whisper/ggml symbols
  (`nm -u | grep -ci 'cuda\|llama\|whisper\|ggml'` → 0). Its make target depends on neither
  `deps-whisper` nor `deps-llama`, so a fresh clone can fetch models while the ~10-minute CUDA
  build runs. Keep it that way.
- **Read on any poll event, not just `POLLIN`.** When curl exits, `poll()` reports `POLLHUP` with
  no `POLLIN`; a `POLLIN`-only loop spins at 100% CPU forever instead of seeing end-of-output.
  Called out in both the CLI loop and the test's driver because D3's window inherits this loop.
- **Reaping is not the end of reading** (found by review, and it is the failure this whole design
  exists to prevent). Anything curl writes between the last `download_read` and its exit is still
  buffered in the pipe when `waitpid` succeeds; a loop that stops on `download_reap` returning 1
  silently drops it — and on a fast failure that dropped text *is* the entire error message. Both
  the CLI loop and the test driver now drain to EOF after reaping. `tests/test_download.c` pins it
  with a deterministic case (let curl exit, sleep, reap *first*, then drain, and assert `curl:`
  survived); removing the post-reap drain makes that assertion fail, which was checked by
  temporarily deleting it rather than assumed. Note the timing-dependent version of the same
  assertion in the normal drive loop passes either way — it is not what guards this.
- **`download_cancel` is bounded**: SIGTERM → `waitpid(WNOHANG)` poll for ~2 s → SIGKILL →
  blocking wait. An unbounded cancel would hang the window's close path and any test that
  exercises it. Verified by timing (< 3 s) and by asserting no zombie is left.
- **`download_reap` is idempotent** (guards on `running`), so a second call can't waitpid a reaped
  pid or rename twice. Exit 0 with no `.part` at all is treated as a *failure*, not a silent
  success. A size mismatch against the catalog only warns — those sizes are advisory and go stale
  when upstream re-uploads; curl considering the transfer complete is the real signal.
- **`download_start` creates dest's parent directory.** A fresh clone has no `models/` at all
  (git doesn't track empty directories), and curl would otherwise fail with a bare "Failed to open
  the file".
- **CR translation** lives in the downloader (`'\r'` → `'\n'`, runs collapsed) with the collapse
  state carried in the struct across reads, so a run split over two `read()`s doesn't emit a blank
  line. Unit-tested on synthetic meter fragments — no child process needed.
- **`signal(SIGPIPE, SIG_IGN)` is in `main()`, not in the library.** A library function mutating
  global signal disposition is the wrong shape; the parent never writes to the pipe anyway.
- **`tests/test_download.c`** (added to `TEST_SRCS`) stays **offline**: transfers use `file://`
  URLs, which curl supports natively. Covers the success path (`.part` promoted to dest, parent
  dir created, `got_bytes` correct, pipe closed, reap idempotent), the failure path (nonzero exit,
  no dest, `.part` removed), and cancellation (a FIFO nobody writes to parks curl in `open()`, so
  the child is reliably alive with zero disk churn). SKIPs if curl is absent, after still checking
  CR translation.
- **`--fail` is HTTP-only, so `file://` cannot exercise it.** Verified by hand against the real
  network instead, and recorded here rather than added to `make test`:
  - bad HuggingFace URL → `curl: (22) The requested URL returned error: 404`, exit 22, **no stub
    file** in the models dir, full curl text on screen and the command echoed for re-running.
  - real HTTPS fetch (a small file from the same repo) → the `-L` redirect and then the file, meter
    readable across lines (CR translation working), landed at dest with no `.part`.
  - a 120 MB `file://` copy with a known catalog size → `progress: 0.0% → 100.0%`, exercising the
    `stat()`-based fraction D3's bar will draw.
  - SIGINT mid-fetch → "interrupted -- cancelling", no `.part`, no stray curl process.
  - **the real thing**: `./dictation-setup --fetch-model base.en`, 141 MiB in 49 s at ~2.9 MB/s,
    meter ticking the whole way, landing at `./models/ggml-base.en.bin` with no `.part`. The
    catalog's advisory size turned out to be **12,746 bytes stale** (147,951,465 from the plan vs
    the real 147,964,211), so the size-mismatch warning fired for real, on its first real
    opportunity, and the file was kept — exactly the intended behaviour. `configs/models.conf` now
    carries the verified size.
- **The base.en fetch un-skipped `test_stt`, which then failed** — and the failure was a real,
  pre-existing gap, not a regression: `WAV_PATH` is `whisper.cpp/samples/jfk.wav`, but the vendored
  whisper.cpp ships only `samples/jfk.mp3` (upstream dropped the wav). The missing-model SKIP
  returns first, so this had been invisible since Phase A — the "test_stt SKIPs" note in the D0/D1
  logs was hiding two problems, not one. Fixed by deriving the wav from the tracked mp3 in a
  `$(SAMPLE_WAV)` make rule (gitignored, `-`-prefixed so a machine without ffmpeg degrades to a
  skip) plus a missing-sample SKIP branch in `test_stt.c` mirroring the model one. **`make test`
  now runs all nine suites green with no skips at all**, and `test_stt: OK` is itself the proof
  that the downloaded model is intact and usable — whisper loads it and returns the expected
  transcript. Consequence worth knowing if a transcript mismatch is ever debugged here: the audio
  is now mp3-derived, so it is not byte-identical to what Phase A transcribed. The assertion is a
  substring check for "country" and was stable across repeated runs, so there is ample margin, but
  it is not the same waveform.
- **One cosmetic fix found by running it**: the periodic `progress:` line spliced itself into the
  middle of a half-written curl meter line. It is now gated on the downloader's `at_line_start`,
  so it only prints at a line boundary.
- **Build wiring, deliberately minimal** (the daemon is in daily use on this branch, so its link
  line is unchanged this commit): new `SETUP_ONLY_SRCS := src/downloader.c` and
  `OBJS_TEST := $(OBJS_NOMAIN) $(SETUP_ONLY_OBJS)`; the test rule now links `OBJS_TEST`. `SRCS` was
  **not** touched — D1's note about `model_catalog.o` being dead weight in the daemon still stands,
  and that cleanup belongs in D3 when `SETUP_SRCS` grows the GUI files. `all` now builds `setup`
  too, and `clean` removes both binaries.
- **Regression**: `make test` exits 0 with `test_download` added (`test_stt` still SKIPs for the
  pre-existing D0 reason). `make app` reports nothing to do, and `./dictation --help` /
  `./dictation-setup --list` both behave — the running daemon is untouched.
- **Still open for D3/D4**: nothing writes `configs/local.conf` yet, so
  `scripts/waybar-dictation.sh`'s hardcoded `configs/example.conf` (D1's noted follow-up) remains
  latent and remains D4's.

### Phase D1 — model catalog + config writer (done)

- **`configs/models.conf`** created with the three seed rows (large-v3-turbo-q5_0, base.en,
  Qwen2.5-1.5B-Instruct Q4_K_M) in the planned `kind|id|display|filename|url|size_bytes` format.
  Sizes: the two locally present files were `stat`ed for real values — large-v3-turbo-q5_0 is
  `574041195` (matches the plan) and the Qwen GGUF is `1117320736` (the plan's `…064` was wrong,
  corrected here). base.en's `147951465` is upstream-sourced and *not* verified locally; the file
  isn't downloaded on this machine. Sizes are advisory anyway (progress bar only).
- **`src/model_catalog.{h,c}`**: `catalog_load` / `catalog_refresh_presence` / `catalog_find` /
  `catalog_free` over a `realloc`-grown `struct model_entry[]`, exactly as specified.
  - **Deliberate divergence from `config.c`: no inline-`#`-comment stripping.** `config.c:122`
    truncates a value at the first `#`; doing that here would silently corrupt any URL with a
    fragment or query, so only a whole-line `#` (after leading whitespace) is a comment. Called
    out in a code comment, since the divergence otherwise reads as an oversight, and covered by a
    test using `…/t.bin?a=1#f`.
  - **Over-long fields are rejected, not truncated.** A `snprintf`-truncated URL is a 404 in D2
    that looks like upstream moved the file; `copy_field()` warns and skips the row instead. Same
    posture for an unknown `kind`. A bad `size_bytes` is the one soft failure — it degrades to `0`
    (unknown) rather than dropping an otherwise usable model.
  - `getline` instead of `config.c`'s fixed `char[1024]`: a record is a URL plus a display name,
    and a silently split line would parse as two malformed ones.
  - `catalog_refresh_presence` checks `snprintf`'s return and treats an overlong
    `<models_dir>/<filename>` as missing, so `-Wformat-truncation` has nothing to complain about.
- **`trim()` duplicated, not exported.** `config.c`'s is file-static; the point of this phase is
  that `model_catalog.o` doesn't have to link `config.o`, so a byte-identical six-line static copy
  lives in `model_catalog.c` with a comment naming its origin. `config_write.c` needs no trim at
  all — it does anchored `[ \t]` scanning instead.
- **`src/config_write.{h,c}`**: `config_write_keys()` (line-preserving rewriter), plus
  `config_bootstrap_local()` and `config_default_path()`.
  - The anchor is `^[ \t]*<key>[ \t]*=` and nothing else, so `example.conf`'s prose comments that
    *mention* `whisper_model_path`, and any `#whisper_model_path=…` line, survive untouched.
  - **Every** matching occurrence is rewritten, not just the first: `config_load` is last-wins, so
    leaving a later duplicate would silently override the value just written.
  - Unhandled keys are appended under `# --- written by dictation-setup ---`, emitted only when
    there is something to append, and a file not ending in `\n` gets a separator first. The result
    is idempotent: a second call finds the appended line and rewrites it in place — asserted
    (exactly one header, one key) rather than assumed.
  - `.tmp` + `rename()`, with `fclose`'s return checked before the rename (buffered write errors
    such as `ENOSPC` surface at flush time — an unchecked one turns "atomic write" into
    "atomically installed a truncated config") and the `.tmp` unlinked on every failure path.
  - `config_bootstrap_local` creates with `O_WRONLY|O_CREAT|O_EXCL` rather than stat-then-copy, so
    seeding can never clobber a config the user edited, even against a concurrent setup process.
    Returns 1=created / 0=already existed / -1=error.
- **`config_default_path()` lives in `config_write.c`, not static in `main.c`**, because D2's
  `setup_main.c` needs the same preference and this makes it testable. `main.c:236` now calls it,
  and the `--help` text was updated to match — a `--help` that still claimed
  `configs/example.conf` would be a lie.
- **Build wiring**: `src/config_write.c` and `src/model_catalog.c` were added to `SRCS`, which is
  the cheapest way to satisfy the existing `tests/%: tests/%.c $(OBJS_NOMAIN)` rule. The daemon
  therefore links `model_catalog.o` without ever calling it (~2 KB of an already-73 MB binary);
  `config_write.o` is genuinely used via `config_default_path()`. The alternative — a separate
  object list just for the two new tests — buys nothing until `dictation-setup` needs its own
  `SETUP_SRCS` in D2/D3, and can be revisited then.
- **Tests**: `tests/test_catalog.c` and `tests/test_config_write.c` added to `TEST_SRCS`. Beyond
  the planned cases they assert that the *shipped* `configs/models.conf` parses (nothing else
  validates those seed rows; note the cwd dependency — `make test` runs from the repo root), that
  a commented-out assignment is not rewritten, that an empty `llama_model_path=` round-trips
  through `config_load` to a cleanup-disabled config (D3's "none (raw whisper)" option), and that
  bootstrapping twice preserves a hand edit.
- **Verification**: `make test` exits 0 — `test_config`, `test_config_write`, `test_catalog`,
  `test_directives`, `test_ipc`, `test_llm`, `test_inject` pass; `test_stt` still SKIPs for the
  pre-existing reason recorded under D0. `make app` and both new test binaries compile
  warning-clean (checked with the test binaries deleted first, so the compile actually ran), and
  `nm -u` on both new objects shows only libc plus `log_msg` — they stay linkable into a
  CUDA-free `dictation-setup`. Confirmed by hand that a `configs/local.conf` naming a bogus model
  makes a bare `./dictation` fail on *that* path, i.e. the new default is really in effect.
- **`.gitignore`**: `configs/local.conf` plus `configs/*.tmp` (the rewriter's scratch file, in case
  a crash ever leaves one behind).
- **Known follow-up for D4, deliberately not fixed here**: `scripts/waybar-dictation.sh:14` still
  passes `configs/example.conf` explicitly, so a script-started daemon and a bare `./dictation`
  can now read *different* files. It is latent — nothing writes `local.conf` until D3 — and that
  script is D4's scope, but it must be done in the same commit that makes the GUI write
  `local.conf`.

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
