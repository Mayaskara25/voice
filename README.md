# dictation — local push-to-talk dictation daemon

Hold a hotkey, speak, release — the transcribed text is typed into whatever window
currently has focus, as if you'd typed it yourself. Everything runs **locally and
offline**: audio never leaves the machine, there are no cloud APIs and no network
dependency. The daemon is a single C program linking directly against
[whisper.cpp](https://github.com/ggml-org/whisper.cpp)'s C API — no Python, no subprocess
orchestration, no HTTP service. A small companion binary, `dictation-setup`, picks and
downloads models and starts/stops the daemon; it links only `-lX11`, and the daemon itself
still contains no network code at all.

Target platform: **Linux** (tested on Ubuntu 24.04 / X11 and Arch / Hyprland). Keystroke injection has two
backends (`inject_backend` in the config): `xtest` (default, X11 only) and `ydotool`
(works under any Wayland compositor too, e.g. Hyprland — X11's XTest events never reach
native Wayland clients). See `PLAN.md` for the full design rationale and the reasoning
behind every architecture decision.

## Privacy

100% local by default. Captured audio is held in memory only for the duration of one
utterance and discarded after transcription; nothing is written to disk or sent anywhere.
Because dictation works by **synthesizing keystrokes into the focused window**, treat it
like any accessibility/keyboard-injection tool: only run it when you intend it to type on
your behalf. `test_mode` prints the transcript to stdout instead of injecting, for safe
experimentation.

## Status

| Phase | What | State |
|-------|------|-------|
| **A** | capture → whisper → inject (headless) | ✅ done |
| **C** | microui/Xlib status GUI (focus-preserving) | ✅ done |
| **B** | optional GPU LLM cleanup via llama.cpp | ✅ done |
| **D** | setup GUI (model picker + downloader) & `.desktop` entry | ✅ done |

## One-time setup

```sh
sudo apt update
sudo apt install -y cmake libxtst-dev        # build-essential, libasound2-dev, libx11-dev,
                                             # libxi-dev are assumed already present
sudo usermod -aG input $USER                 # then LOG OUT AND BACK IN (newgrp won't propagate)
```

### Recommended: the setup window

```sh
make setup          # builds ./dictation-setup only -- seconds, not minutes
./dictation-setup
```

Pick a speech model and (optionally) a cleanup model, press **[Get]** on anything marked
`MISSING` to download it with a live progress bar and the raw `curl` output in view, then
press **Start dictation**. Your choices are written to `configs/local.conf`, which is
seeded from `configs/example.conf` so it keeps every comment and tuned setting; both the
daemon and `scripts/waybar-dictation.sh` prefer that file when it exists.

Pressing **Start dictation** with a selected model that hasn't been downloaded yet opens a
prompt offering to fetch it, rather than starting a daemon that would fail on the missing
file. This covers the cleanup model too — a cleanup model is optional, but once one is
*selected*, the daemon treats a missing file as fatal.

**[Remove]** appears where **[Get]** does, on models that are already downloaded, and asks
for confirmation first. The confirmation names every file it will delete, because that is
not always the path shown in the list: models under `models/` are often symlinks into
`whisper.cpp/models/` (that's what the `ln -sf` step below creates), so removing only the
link would free nothing. The real file is deleted too **when it lives inside the project**;
a symlink pointing outside has only its link removed, and the dialog says so and reports
0 MB freed. Removing the model your config currently names is allowed, but it's called out
in the dialog — Super+D would fail until you pick another or re-download it.

Removing and re-downloading the same model repeatedly does **not** accumulate disk usage;
each removal frees what the previous download took. Note that after the first
remove/re-download cycle the model is a plain file under `models/` rather than a symlink,
so it no longer occupies space inside `whisper.cpp/models/`.

**Cleanup models are loaded onto the GPU** (`n_gpu_layers=99`), so their file size is
roughly the VRAM they need, plus ~300 MB of compute buffer and KV cache. The listed sizes
are there to be read before committing to a multi-GB download: on a 4 GB card, the 2.3 GB
Gemma 3n E2B Q3_K_M fits comfortably where the 2.8 GB Q4_K_M is tight. Speech models run
on the CPU backend and cost RAM, not VRAM.

`make setup` deliberately builds **without whisper, llama or CUDA** — it links only
`-lX11`. So on a fresh clone you can start the ~10-minute dependency build (`make`) in one
terminal and download models in the setup window at the same time.

The list of available models lives in [`configs/models.conf`](configs/models.conf) as
plain `kind|id|display|filename|url|size_bytes` records, so when an upstream URL moves
that is a one-line data edit rather than a recompile.

### Headless alternative (ssh, or no X display)

The same downloader without a window — this is also the path to use if you'd rather not
run a GUI at all:

```sh
./dictation-setup --list                    # what's available, and what's already present
./dictation-setup --fetch-model base.en     # download one entry by id
```

### Manual model download (fallback)

Nothing above is required; models can still be fetched by hand and referenced directly
from a config:

```sh
# base.en = faster, less accurate; large-v3-turbo = what example.conf points at
./whisper.cpp/models/download-ggml-model.sh base.en
ln -sf ../whisper.cpp/models/ggml-base.en.bin models/ggml-base.en.bin

./whisper.cpp/models/download-ggml-model.sh large-v3-turbo
```

### Optional: desktop entry (app launcher)

```sh
make install-desktop      # -> ~/.local/share/applications/dictation.desktop
```

"Dictation" then appears in the desktop app search menu like any installed application.
Per-user, so **no root**; `make uninstall-desktop` removes it. The entry's `Exec=` is an
absolute path generated at install time from the current checkout, which is why the
tracked file is a template (`dictation.desktop.in`) — re-run `make install-desktop` if you
move the repository.

Launched this way the process starts with `cwd=$HOME`, so it locates the project from its
own executable path and changes there before reading anything; `--dir PATH` overrides.

Note that `Terminal=false` means startup errors (no X display, unreadable catalog) are
logged where a launcher gives you nowhere to look — check the compositor's stderr or
`journalctl --user` if clicking the icon appears to do nothing.

### Optional: Wayland support (`inject_backend=ydotool`)

XTest (the default injection backend) only reaches X11/XWayland-backed windows. Under a
Wayland compositor (Hyprland, GNOME, KDE/Wayland, ...) native Wayland clients silently
never receive the injected keystrokes at all — this isn't a bug to work around, it's
Wayland's input-isolation security model. To inject there instead, use `ydotool`, which
types via `/dev/uinput` at the kernel level and works under any compositor:

**This limit applies only to keystroke injection, not to the windows.** The setup window
and the status panel are ordinary X11 clients and render through XWayland on any
compositor — Hyprland included, where both are developed and tested. Only *synthesizing
input into someone else's window* is what Wayland forbids.

```sh
sudo pacman -S ydotool      # Arch (official 'extra' repo)
sudo apt install ydotool    # Ubuntu -- verify it's packaged for your release
```

**Recommended: auto-start ydotoold via systemd user service** (starts automatically on login,
no manual terminal needed):

```sh
systemctl --user enable --now ydotool.service
systemctl --user status ydotool.service --no-pager  # verify it's running
```

Then set `inject_backend=ydotool` in your config. `./dictation` checks at startup that
the `ydotool` binary and a running `ydotoold` are both reachable (via the same
`YDOTOOL_SOCKET`/`$XDG_RUNTIME_DIR` resolution the CLI uses), and fails fast with a
specific error if not, rather than silently doing nothing per keystroke.

**(Legacy: manual invocation)** If your distro doesn't package the systemd service, run
ydotoold manually in a separate terminal — the daemon socket must be pinned to match where
the CLI/app expect it:
```sh
sudo ydotoold --socket-path="/run/user/$(id -u)/.ydotool_socket" \
              --socket-own="$(id -u):$(id -g)" --socket-perm=0600 &
```

### Optional: on-demand start/stop via a Hyprland keybind + Waybar indicator

Rather than leaving the daemon running (and its models resident on the GPU) for the
whole session, `scripts/waybar-dictation.sh` starts/stops it on demand:

```sh
scripts/waybar-dictation.sh start    # launch headless, detached
scripts/waybar-dictation.sh stop     # SIGINT -- same clean shutdown as Ctrl-C
scripts/waybar-dictation.sh toggle   # start if not running, else stop
scripts/waybar-dictation.sh status   # prints Waybar-compatible JSON: notactive/loading/active
```

`status` distinguishes "process running but models still loading" from "ready" by
grepping the log for the `dictation: ready` line, so a UI indicator can show a
loading state rather than jumping straight to "on". Process identity is tracked by
name (`pgrep -x`/`pkill -INT -x dictation`), not a PID file — capturing `$!` across a
`cd dir && nohup bin &` background job is unreliable (it can catch an intermediate
shell PID instead of the exec'd binary's), which was confirmed the hard way during
development.

To wire this into Hyprland + Waybar (tested with an ml4w-dotfiles setup):
- **Keybind**: bind a key to `scripts/waybar-dictation.sh toggle` (e.g. `SUPER + D`).
- **Waybar module**: a `custom` module with `"return-type": "json"`, `"exec"` set to
  `.../waybar-dictation.sh status`, `"on-click"` set to `.../waybar-dictation.sh toggle`,
  plus a `"signal"` number so the toggle script can `pkill -RTMIN+N waybar` for an
  instant refresh instead of waiting on a polling interval.
- **If you're on ml4w-dotfiles**: don't edit its `default.lua` / `modules.json`
  directly — ml4w's updater re-stages your whole profile from a fresh clone on
  update, and only restores files it explicitly offers to keep. Put the keybind and
  module definition in new files instead (e.g. `conf/keybindings/my.lua`,
  `waybar/custom-dictation.json`) and just point the small stub
  (`conf/keybinding.lua`) / theme `include` array at them. New files are never
  touched by the update's restaging (it only overwrites paths that exist in the
  fresh clone); only add the tiny stub/pointer files to
  `~/.config/ml4w-dotfiles-installer/<profile-id>/blacklist` so ml4w's updater
  leaves your one added line alone.

### Optional: LLM cleanup (Phase B, GPU)

The transcript can be passed through a local LLM (llama.cpp) to fix casing/punctuation
before injection. This is **GPU-accelerated** (CUDA) and needs the CUDA toolkit plus a model:

```sh
sudo apt install -y nvidia-cuda-toolkit          # provides nvcc (needs the NVIDIA driver)
# download the cleanup model (~1 GB) into models/
wget -O models/Qwen2.5-1.5B-Instruct-Q4_K_M.gguf \
  https://huggingface.co/bartowski/Qwen2.5-1.5B-Instruct-GGUF/resolve/main/Qwen2.5-1.5B-Instruct-Q4_K_M.gguf
```

Then set `llama_model_path` in the config (already set in `example.conf`). Leaving it **blank
disables cleanup at runtime** (raw whisper output). Note: the current build always links
llama.cpp + CUDA (`make` builds `deps-llama`), so the toolkit is required to build even if you
run with cleanup off; a compile-time opt-out is possible future work. `n_gpu_layers=0` runs
the model on CPU (much slower). The vendored `llama.cpp` is pinned so its bundled ggml (0.15.3)
matches whisper's, letting both share one ggml backend.

**Expected startup output on CUDA systems:** You'll see `load_tensors: offloaded 27/29 layers to GPU` and
`load_tensors: CPU_Mapped model buffer size = 125 MiB`. The embedding table (`token_embd.weight`)
stays in CPU memory by design — it's a lookup, not compute-intensive. This preserves ~125 MB of VRAM
with negligible latency impact, while the actual 29 transformer layers (the computational bottleneck)
remain fully GPU-accelerated. This is expected and optimal.

### Microphone gain / capture device (read this — it bit us)

Two non-obvious, machine-specific gotchas found during development (details in `PLAN.md`):

- **Mic gain:** default ALSA `Capture` / `Mic Boost` levels may be maxed out (+30 dB each),
  clipping 100% of captured audio regardless of the code. Record a few seconds and check the
  signal isn't pinned at full scale throughout:
  ```sh
  arecord -d 3 -f S16_LE -r 16000 -c 1 /tmp/check.wav   # then listen with: aplay /tmp/check.wav
  amixer -c 1 sset 'Mic Boost' 0        # control names vary; list with: amixer -c <N> scontrols
  amixer -c 1 sset 'Capture' 40%
  sudo alsactl store
  ```
- **Capture device:** the app deliberately uses `plughw:<card>,<dev>`, **not** PipeWire's
  `"default"` node, which was found to corrupt/clip 16 kHz mono capture on this machine.
  Auto-detection picks the first capture-capable card; override with `audio_device` if you
  have more than one (e.g. a USB webcam mic). List cards with `arecord -l`.

## Build

```sh
make            # builds whisper.cpp + llama.cpp (CMake sub-builds, CUDA) + both binaries
make setup      # builds ONLY ./dictation-setup -- no whisper, no llama, no CUDA, seconds
make test       # unit tests: config parser, self-pipe handoff, whisper on jfk.wav, LLM cleanup
```

`make` targets: `all`, `app`, `setup`, `deps-whisper`, `deps-llama`, `test`, `run`,
`list-keys`, `install-desktop`, `uninstall-desktop`, `clean`,
`distclean` (also removes the slow-to-rebuild vendored `build/` dirs). The
`deps-llama` sub-build compiles llama.cpp with `-DGGML_CUDA=ON -DCMAKE_CUDA_ARCHITECTURES=75`
and `-DGGML_CUDA_FORCE_MMQ=ON`. The second flag is needed for Turing GPUs without tensor cores
(e.g. GTX 1650) to force integer matmul kernels instead of a degraded tensor-core fallback path.

**CUDA library paths:** The Makefile auto-derives `CUDA_HOME` from `nvcc`'s location. This works
around a distro packaging difference: Ubuntu's `nvidia-cuda-toolkit` drops runtime libs
(`libcudart`, `libcublas`) into default linker paths, while Arch's `cuda` package keeps them under
`$CUDA_HOME/lib64`. The Makefile detects and adds the necessary `-L` flags automatically.

LLM cleanup is optional at **runtime** (blank `llama_model_path`), but the build
currently links llama.cpp + CUDA unconditionally.

## Run

```sh
./dictation --list-keys                    # press your desired PTT key; note its evdev code + device
./dictation --config configs/example.conf  # run the daemon (edit the config first)
./dictation --test-mode                    # print transcripts instead of injecting
./dictation --gui                          # also show the status panel (or set gui_enabled=true)
```

With no `--config`, the daemon reads `configs/local.conf` when it exists and falls back to
`configs/example.conf` — the same resolution `dictation-setup` and
`scripts/waybar-dictation.sh` use, so all three always agree on which file is in effect.

Then: focus any text editor, **hold** the configured PTT key, speak, **release** — the text
appears in the editor a second or two later. Ctrl-C to quit.

### Spoken per-utterance directives

Say one of these keywords at the start of an utterance to override formatting for just that
clip:

| Say | Result |
|-----|--------|
| `keep all lowercase i am hungry` | types `i am hungry` and skips LLM cleanup |
| `spell word c o d e x` or `spell c-o-d-e-x` | types `codex` and skips LLM cleanup |
| `literal codex --help` / `no cleanup codex --help` | types the remaining text without cleanup |
| `command echo hello` | cleans the remaining text with `cleanup_style=commands` for this utterance |
| `code mode const name equals codex` | cleans the remaining text with `cleanup_style=code` for this utterance |

Angle-bracket tags such as `<keep all lowercase>` and `<spell>` are also accepted if the
transcript contains them, but the spoken forms above are the intended workflow.

The `--gui` status panel is an **override-redirect** window that never takes input focus, so
your keystrokes still land in the app you were looking at while the panel shows the pipeline
state (`idle → recording → transcribing → cleaning → injecting`; `cleaning` appears only when
LLM cleanup is enabled).

## Configuration

Key = value, `#` starts a comment (see `configs/example.conf`):

| Key | Meaning |
|-----|---------|
| `whisper_model_path` | path to the ggml whisper model (default in `example.conf`: `./models/ggml-large-v3-turbo-q5_0.bin`) |
| `llama_model_path` | LLM cleanup model (GGUF); **blank = cleanup disabled** (raw whisper output) |
| `cleanup_style` | `dictation` (default) / `code` / `commands`; only used when cleanup is on |
| `n_gpu_layers` | LLM GPU offload: `99` = all layers on GPU, `0` = CPU-only |
| `ptt_device` | evdev device (e.g. `/dev/input/event3`); blank = auto-detect. **Note:** with `inject_backend=ydotool`, auto-detect can pick up ydotoold's virtual uinput device instead of your physical keyboard — set this explicitly to avoid it. |
| `ptt_keycode` | **evdev** key code (from `--list-keys`), *not* an X keysym; default 97 = RIGHTCTRL |
| `audio_device` | ALSA capture device (e.g. `plughw:2,0`); blank = auto-detect |
| `n_threads` | whisper CPU threads (also the LLM's CPU threads) |
| `whisper_use_gpu` | `true` = run whisper on the GPU (shares the CUDA backend; CPU fallback if none) |
| `language` | fixed `en` for now |
| `test_mode` | `true` = print instead of inject |
| `gui_enabled` | `true` = show the status panel |
| `gui_font` | X core font for the panel (XLFD/alias, e.g. `9x15`); blank = `fixed` |
| `inject_backend` | `xtest` (default, X11 only) or `ydotool` (works on any Wayland compositor too) |

> **evdev codes vs X keysyms are different numberspaces** — `ptt_keycode` is always the
> evdev code reported by `--list-keys`, never an X keycode.

## Vendored dependencies

Included as flat source (upstream `.git` removed) under MIT licenses:

- `whisper.cpp/` — `v1.9.1-81-g6fc7c33b`
- `microui/` — `0850aba` (v2.02)
- `llama.cpp/` — `96183e982` (pinned so its bundled ggml 0.15.3 matches whisper's, so both
  share one ggml backend; llama's CUDA-enabled ggml serves whisper's CPU path too)

## Layout

```
src/            application C sources (see PLAN.md "Module breakdown")
configs/        example.conf (tracked), models.conf (the model catalog),
                local.conf (gitignored; written by dictation-setup)
models/         gitignored; downloaded whisper and llama models
tests/          assert-based unit tests (make test)
scripts/        start/stop/status helper for Hyprland keybind + Waybar integration
dictation.desktop.in
                app-launcher entry template; `make install-desktop` fills in the path
PLAN.md         the living design doc / source of truth across all phases
```
