CC       ?= gcc
CSTD     := -std=gnu11
CFLAGS   ?= -Wall -Wextra -O2
CPPFLAGS := $(CSTD) -I src -I whisper.cpp/include -I whisper.cpp/ggml/include \
            -I llama.cpp/include -I microui/src

WHISPER_BUILD := whisper.cpp/build
WHISPER_LIB   := $(WHISPER_BUILD)/src/libwhisper.a

LLAMA_BUILD := llama.cpp/build
LLAMA_LIB   := $(LLAMA_BUILD)/src/libllama.a

SRCS := src/main.c src/config.c src/log.c src/hotkey_evdev.c src/audio_alsa.c \
        src/stt_whisper.c src/inject_xtest.c src/llm_cleanup.c \
        src/ipc_handoff.c src/font_xlib.c src/gui_xlib.c \
        microui/src/microui.c
OBJS := $(SRCS:.c=.o)

# Everything except the app entry point, for linking test binaries.
OBJS_NOMAIN := $(filter-out src/main.o,$(OBJS))

TEST_SRCS := tests/test_config.c tests/test_ipc.c tests/test_stt.c tests/test_llm.c
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
          -lcudart -lcublas -lcuda \
          -lasound -lX11 -lXtst -lXi -lpthread -lstdc++ -lm -fopenmp

BIN := dictation

.PHONY: all deps-whisper deps-llama app test clean distclean run list-keys

all: deps-whisper deps-llama app

deps-whisper: $(WHISPER_LIB)

$(WHISPER_LIB):
	cmake -B $(WHISPER_BUILD) -S whisper.cpp \
		-DBUILD_SHARED_LIBS=OFF -DWHISPER_SDL2=OFF -DGGML_CUDA=OFF -DGGML_VULKAN=OFF \
		-DCMAKE_BUILD_TYPE=Release
	cmake --build $(WHISPER_BUILD) -j$$(nproc) --target whisper

# Phase B
deps-llama: $(LLAMA_LIB)

$(LLAMA_LIB):
	cmake -B $(LLAMA_BUILD) -S llama.cpp \
		-DBUILD_SHARED_LIBS=OFF -DGGML_CUDA=ON -DGGML_VULKAN=OFF \
		-DCMAKE_CUDA_ARCHITECTURES=75 -DLLAMA_CURL=OFF \
		-DCMAKE_BUILD_TYPE=Release
	cmake --build $(LLAMA_BUILD) -j$$(nproc) --target llama

app: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDLIBS)

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

# --- tests: small assert-based programs, no framework ---
test: deps-whisper deps-llama $(TEST_BINS)
	@rc=0; for t in $(TEST_BINS); do \
		echo "=== $$t ==="; ./$$t || rc=1; \
	done; exit $$rc

$(TEST_BINS): tests/%: tests/%.c $(OBJS_NOMAIN)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(OBJS_NOMAIN) -o $@ $(LDLIBS)

clean:
	rm -f $(OBJS) $(BIN) $(TEST_BINS)

distclean: clean
	rm -rf $(WHISPER_BUILD) $(LLAMA_BUILD)

run: app
	./$(BIN) --config configs/example.conf

list-keys: app
	./$(BIN) --list-keys
