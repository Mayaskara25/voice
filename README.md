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
| **B** | optional LLM cleanup via llama.cpp | ⏳ planned (see `PLAN.md`) |

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
make            # builds whisper.cpp (CMake sub-build) + the dictation binary
make test       # runs the unit tests (config parser, self-pipe handoff, whisper on jfk.wav)
```

`make` targets: `all`, `app`, `deps-whisper`, `test`, `run`, `list-keys`, `clean`,
`distclean` (also removes the slow-to-rebuild vendored `build/` dirs).

## Run

```sh
./dictation --list-keys                    # press your desired PTT key; note its evdev code + device
./dictation --config configs/example.conf  # run the daemon (edit the config first)
./dictation --test-mode                    # print transcripts instead of injecting
./dictation --gui                          # also show the status panel (or set gui_enabled=true)
```

Then: focus any text editor, **hold** the configured PTT key, speak, **release** — the text
appears in the editor a second or two later. Ctrl-C to quit.

The `--gui` status panel is an **override-redirect** window that never takes input focus, so
your keystrokes still land in the app you were looking at while the panel shows the pipeline
state (`idle → recording → transcribing → injecting`).

## Configuration

Key = value, `#` starts a comment (see `configs/example.conf`):

| Key | Meaning |
|-----|---------|
| `whisper_model_path` | path to the ggml whisper model (default `./models/ggml-base.en.bin`) |
| `llama_model_path` | Phase B only; blank = no LLM cleanup |
| `ptt_device` | evdev device (e.g. `/dev/input/event3`); blank = auto-detect |
| `ptt_keycode` | **evdev** key code (from `--list-keys`), *not* an X keysym; default 97 = RIGHTCTRL |
| `audio_device` | ALSA capture device (e.g. `plughw:2,0`); blank = auto-detect |
| `n_threads` | whisper CPU threads |
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

## Layout

```
src/            application C sources (see PLAN.md "Module breakdown")
configs/        example.conf
models/         gitignored; downloaded whisper (and, later, llama) models
tests/          assert-based unit tests (make test)
PLAN.md         the living design doc / source of truth across all phases
```
