# dictation — local push-to-talk dictation daemon

Hold a hotkey, speak, release — the transcribed text is typed into whatever window
currently has focus, as if you'd typed it yourself. Everything runs **locally and
offline**: audio never leaves the machine, there are no cloud APIs and no network
dependency. The whole thing is a single C program linking directly against
[whisper.cpp](https://github.com/ggml-org/whisper.cpp)'s C API — no Python, no subprocess
orchestration, no HTTP service.

Target platform: **Linux / X11** (developed on Ubuntu 24.04). See `PLAN.md` for the full
design rationale and the reasoning behind every architecture decision.

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

## One-time setup

```sh
sudo apt update
sudo apt install -y cmake libxtst-dev        # build-essential, libasound2-dev, libx11-dev,
                                             # libxi-dev are assumed already present
sudo usermod -aG input $USER                 # then LOG OUT AND BACK IN (newgrp won't propagate)

# download a whisper model (base.en = better accuracy, tiny.en = ~2x lower latency)
./whisper.cpp/models/download-ggml-model.sh base.en
ln -sf ../whisper.cpp/models/ggml-base.en.bin models/ggml-base.en.bin
```

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
make            # builds whisper.cpp + llama.cpp (CMake sub-builds, CUDA) + the dictation binary
make test       # unit tests: config parser, self-pipe handoff, whisper on jfk.wav, LLM cleanup
```

`make` targets: `all`, `app`, `deps-whisper`, `deps-llama`, `test`, `run`, `list-keys`,
`clean`, `distclean` (also removes the slow-to-rebuild vendored `build/` dirs). The
`deps-llama` sub-build compiles llama.cpp with `-DGGML_CUDA=ON -DCMAKE_CUDA_ARCHITECTURES=75`
(GTX 1650). LLM cleanup is optional at **runtime** (blank `llama_model_path`), but the build
currently links llama.cpp + CUDA unconditionally.

## Run

```sh
./dictation --list-keys                    # press your desired PTT key; note its evdev code + device
./dictation --config configs/example.conf  # run the daemon (edit the config first)
./dictation --test-mode                    # print transcripts instead of injecting
./dictation --gui                          # also show the status panel (or set gui_enabled=true)
```

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
| `whisper_model_path` | path to the ggml whisper model (default `./models/ggml-base.en.bin`) |
| `llama_model_path` | LLM cleanup model (GGUF); **blank = cleanup disabled** (raw whisper output) |
| `cleanup_style` | `dictation` (default) / `code` / `commands`; only used when cleanup is on |
| `n_gpu_layers` | LLM GPU offload: `99` = all layers on GPU, `0` = CPU-only |
| `ptt_device` | evdev device (e.g. `/dev/input/event3`); blank = auto-detect |
| `ptt_keycode` | **evdev** key code (from `--list-keys`), *not* an X keysym; default 97 = RIGHTCTRL |
| `audio_device` | ALSA capture device (e.g. `plughw:2,0`); blank = auto-detect |
| `n_threads` | whisper CPU threads (also the LLM's CPU threads) |
| `whisper_use_gpu` | `true` = run whisper on the GPU (shares the CUDA backend; CPU fallback if none) |
| `language` | fixed `en` for now |
| `test_mode` | `true` = print instead of inject |
| `gui_enabled` | `true` = show the status panel |
| `gui_font` | X core font for the panel (XLFD/alias, e.g. `9x15`); blank = `fixed` |

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
configs/        example.conf
models/         gitignored; downloaded whisper (and, later, llama) models
tests/          assert-based unit tests (make test)
PLAN.md         the living design doc / source of truth across all phases
```
