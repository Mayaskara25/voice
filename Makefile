CC       ?= gcc
CSTD     := -std=gnu11
CFLAGS   ?= -Wall -Wextra -O2
CPPFLAGS := $(CSTD) -I src -I whisper.cpp/include -I whisper.cpp/ggml/include \
            -I llama.cpp/include -I microui/src

WHISPER_BUILD := whisper.cpp/build
WHISPER_LIB   := $(WHISPER_BUILD)/src/libwhisper.a

LLAMA_BUILD := llama.cpp/build
LLAMA_LIB   := $(LLAMA_BUILD)/src/libllama.a

# CUDA toolkit layout differs by distro (e.g. Ubuntu's nvidia-cuda-toolkit drops
# libcudart/libcublas into a default linker dir; Arch's cuda package keeps them
# under $CUDA_HOME/lib64, which plain `ld` never searches without an explicit
# -L). Derive it from nvcc's location so both work.
CUDA_HOME  := $(shell dirname $$(dirname $$(command -v nvcc 2>/dev/null)) 2>/dev/null)
CUDA_LIBDIR := $(if $(CUDA_HOME),-L$(CUDA_HOME)/lib64 -L$(CUDA_HOME)/lib)

SRCS := src/main.c src/config.c src/config_write.c src/model_catalog.c \
        src/log.c src/hotkey_evdev.c src/audio_alsa.c \
        src/stt_whisper.c src/inject_xtest.c src/inject_ydotool.c src/inject.c \
        src/llm_cleanup.c src/llm_styles.c \
        src/dictation_directives.c \
        src/ipc_handoff.c src/font_xlib.c src/gui_xlib.c \
        microui/src/microui.c
OBJS := $(SRCS:.c=.o)

# Everything except the app entry point, for linking test binaries.
OBJS_NOMAIN := $(filter-out src/main.o,$(OBJS))

# Compiled into dictation-setup (and its tests) but never into the daemon.
# font_xlib.c and microui.c are deliberately NOT here: they are already in SRCS,
# and listing the same object twice on a link line is a duplicate-symbol error.
SETUP_ONLY_SRCS := src/downloader.c src/setup_gui.c
SETUP_ONLY_OBJS := $(SETUP_ONLY_SRCS:.c=.o)

# Test binaries link the daemon's objects plus the setup-only ones, so a test
# can exercise either side without the daemon growing code it never calls.
OBJS_TEST := $(OBJS_NOMAIN) $(SETUP_ONLY_OBJS)

# Phase D: the model picker/downloader, a separate executable because
# scripts/waybar-dictation.sh identifies the daemon by name (pgrep -x dictation).
# Deliberately depends on NEITHER deps-whisper NOR deps-llama -- that is the
# payoff of the D0 llm_styles split, and is what lets a fresh clone fetch models
# before sitting through the ~10-minute CUDA dependency build. Keep it that way:
# nothing here may pull in whisper.cpp, llama.cpp or CUDA.
SETUP_BIN  := dictation-setup
SETUP_SRCS := src/setup_main.c src/setup_gui.c src/downloader.c src/model_catalog.c \
              src/config_write.c src/config.c src/llm_styles.c src/log.c \
              src/font_xlib.c microui/src/microui.c
SETUP_OBJS := $(SETUP_SRCS:.c=.o)
# -lX11 only: no whisper, no llama, no CUDA, and no -lcurl (curl is a child
# process, not a link dependency).
SETUP_LDLIBS := -lX11

# Phase D5: the XDG desktop entry. Committed as a .in template because Exec=
# must be an absolute path, which is machine-specific -- it is generated at
# install time rather than baked into a tracked file. ?= so a caller's
# XDG_DATA_HOME wins (make imports the environment).
XDG_DATA_HOME ?= $(HOME)/.local/share
DESKTOP_DIR   := $(XDG_DATA_HOME)/applications
DESKTOP_FILE  := $(DESKTOP_DIR)/dictation.desktop

TEST_SRCS := tests/test_config.c tests/test_config_write.c tests/test_catalog.c \
             tests/test_download.c tests/test_setup.c \
             tests/test_directives.c tests/test_ipc.c tests/test_stt.c tests/test_llm.c tests/test_inject.c
TEST_BINS := $(TEST_SRCS:.c=)

# One shared ggml: llama's CUDA-enabled 0.15.3 serves BOTH whisper (CPU backend,
# use_gpu=false) and llama (CUDA backend). We link libwhisper.a but NOT whisper's
# ggml. --start-group resolves the circular refs among the ggml archives; CUDA
# runtime libs follow ggml-cuda. (Exact set was confirmed by the Step-1 probe.)
LLAMA_GGML := $(LLAMA_BUILD)/ggml/src
LDLIBS := -L$(WHISPER_BUILD)/src -lwhisper \
          -L$(LLAMA_BUILD)/src -lllama \
          -L$(LLAMA_GGML) -L$(LLAMA_GGML)/ggml-cuda \
          -Wl,--start-group -lggml -lggml-base -lggml-cpu -lggml-cuda -Wl,--end-group \
          $(CUDA_LIBDIR) -lcudart -lcublas -lcuda \
          -lasound -lX11 -lXtst -lXi -lpthread -lstdc++ -lm -fopenmp

BIN := dictation

.PHONY: all deps-whisper deps-llama app setup test clean distclean run list-keys \
        install-desktop uninstall-desktop

all: deps-whisper deps-llama app setup

deps-whisper: $(WHISPER_LIB)

$(WHISPER_LIB):
	cmake -B $(WHISPER_BUILD) -S whisper.cpp \
		-DBUILD_SHARED_LIBS=OFF -DWHISPER_SDL2=OFF -DGGML_CUDA=OFF -DGGML_VULKAN=OFF \
		-DCMAKE_BUILD_TYPE=Release
	cmake --build $(WHISPER_BUILD) -j$$(nproc) --target whisper

# Phase B
deps-llama: $(LLAMA_LIB)

$(LLAMA_LIB):
	# GGML_CUDA_FORCE_MMQ: the GTX 1650 reports as Turing (sm_75) but, unlike
	# RTX 20-series Turing cards, has no tensor cores, so ggml's default
	# tensor-core matmul path runs in a degraded fallback here. This forces
	# ggml to always use its non-tensor-core integer (MMQ) matmul kernels
	# instead. (ggml's own suggestion also proposed retargeting the build to
	# the tensor-core-less Pascal virtual architecture, but this CUDA 13.3
	# toolkit has dropped compute_61 support entirely -- confirmed via `nvcc
	# --list-gpu-arch` -- so sm_75, the real and only viable target here,
	# stays.) This ggml/llama.cpp build is the single shared CUDA backend used
	# by both llama and whisper (see LDLIBS below).
	cmake -B $(LLAMA_BUILD) -S llama.cpp \
		-DBUILD_SHARED_LIBS=OFF -DGGML_CUDA=ON -DGGML_VULKAN=OFF \
		-DCMAKE_CUDA_ARCHITECTURES=75 -DGGML_CUDA_FORCE_MMQ=ON \
		-DLLAMA_CURL=OFF -DCMAKE_BUILD_TYPE=Release
	cmake --build $(LLAMA_BUILD) -j$$(nproc) --target llama

app: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDLIBS)

setup: $(SETUP_BIN)

$(SETUP_BIN): $(SETUP_OBJS)
	$(CC) $(SETUP_OBJS) -o $@ $(SETUP_LDLIBS)

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

# --- tests: small assert-based programs, no framework ---
# test_stt decodes 16-bit PCM, but the vendored whisper.cpp ships samples/jfk.mp3
# (upstream dropped the .wav), so derive the wav from it. Generated, not
# committed: gitignored, and `-` so a machine without ffmpeg just skips the test
# rather than failing the build.
SAMPLE_WAV := whisper.cpp/samples/jfk.wav

$(SAMPLE_WAV): whisper.cpp/samples/jfk.mp3
	-@if command -v ffmpeg >/dev/null 2>&1; then \
		echo "ffmpeg -> $@"; \
		ffmpeg -v error -y -i $< -ar 16000 -ac 1 -c:a pcm_s16le $@; \
	else \
		echo "note: ffmpeg not found -- test_stt will skip (no $@)"; \
	fi

test: deps-whisper deps-llama $(SAMPLE_WAV) $(TEST_BINS)
	@rc=0; for t in $(TEST_BINS); do \
		echo "=== $$t ==="; ./$$t || rc=1; \
	done; exit $$rc

$(TEST_BINS): tests/%: tests/%.c $(OBJS_TEST)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(OBJS_TEST) -o $@ $(LDLIBS)

clean:
	rm -f $(OBJS) $(SETUP_OBJS) $(SETUP_ONLY_OBJS) $(BIN) $(SETUP_BIN) $(TEST_BINS)

distclean: clean
	rm -rf $(WHISPER_BUILD) $(LLAMA_BUILD)

run: app
	./$(BIN) --config configs/example.conf

list-keys: app
	./$(BIN) --list-keys

# --- desktop entry (Phase D5) ---
# Per-user install under $XDG_DATA_HOME, so no root and no system directories.
#
# Depends on $(SETUP_BIN), deliberately NOT on `all`: `all` pulls deps-whisper
# and deps-llama, which would drag the ~10-minute CUDA build into the one target
# that exists to be usable before it. That is the whole D0 payoff. It also means
# the installed entry can never point at a binary that was never built.
#
# Exec= resolves to $(CURDIR), i.e. this build tree, because there is no
# `make install` for the binaries: models/ and configs/ live here, and
# resolve_project_dir() in src/setup_main.c finds the project relative to the
# executable. An entry pointing anywhere else would break that chdir.
install-desktop: $(SETUP_BIN)
	@mkdir -p "$(DESKTOP_DIR)"
	sed 's|@BINDIR@|$(CURDIR)|g' dictation.desktop.in > "$(DESKTOP_FILE)"
	@# No-op without a MimeType=, but harmless and expected by desktop tooling.
	@command -v update-desktop-database >/dev/null 2>&1 && \
		update-desktop-database "$(DESKTOP_DIR)" || true
	@echo "installed $(DESKTOP_FILE)  ->  Exec=$(CURDIR)/$(SETUP_BIN)"

uninstall-desktop:
	rm -f "$(DESKTOP_FILE)"
	@command -v update-desktop-database >/dev/null 2>&1 && [ -d "$(DESKTOP_DIR)" ] && \
		update-desktop-database "$(DESKTOP_DIR)" || true
	@echo "removed $(DESKTOP_FILE)"
