CC       ?= gcc
CSTD     := -std=gnu11
CFLAGS   ?= -Wall -Wextra -O2
CPPFLAGS := $(CSTD) -I src -I whisper.cpp/include -I whisper.cpp/ggml/include -I microui/src

WHISPER_BUILD := whisper.cpp/build
WHISPER_LIB   := $(WHISPER_BUILD)/src/libwhisper.a

LLAMA_BUILD := llama.cpp/build
LLAMA_LIB   := $(LLAMA_BUILD)/src/libllama.a

SRCS := src/main.c src/config.c src/log.c src/hotkey_evdev.c src/audio_alsa.c \
        src/stt_whisper.c src/inject_xtest.c
OBJS := $(SRCS:.c=.o)

LDLIBS := -L$(WHISPER_BUILD)/src -L$(WHISPER_BUILD)/ggml/src \
          -lwhisper -lggml -lggml-cpu -lggml-base \
          -lasound -lX11 -lXtst -lXi -lpthread -lstdc++ -lm -fopenmp

BIN := dictation

.PHONY: all deps-whisper deps-llama app clean distclean run list-keys

all: deps-whisper app

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
		-DBUILD_SHARED_LIBS=OFF -DGGML_CUDA=OFF -DGGML_VULKAN=OFF \
		-DCMAKE_BUILD_TYPE=Release
	cmake --build $(LLAMA_BUILD) -j$$(nproc) --target llama

app: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDLIBS)

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(BIN)

distclean: clean
	rm -rf $(WHISPER_BUILD) $(LLAMA_BUILD)

run: app
	./$(BIN) --config configs/example.conf

list-keys: app
	./$(BIN) --list-keys
