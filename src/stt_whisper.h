#ifndef DICTATION_STT_WHISPER_H
#define DICTATION_STT_WHISPER_H

#include <stddef.h>

struct whisper_context;

struct stt_context {
    struct whisper_context *whisper_ctx;
    int n_threads;
};

/* Loads the whisper model at model_path (CPU-only). Returns 0 on success, -1 on error. */
int stt_init(struct stt_context *stt, const char *model_path, int n_threads);

/* Transcribes normalized float32 mono 16kHz samples. Returns a newly
 * malloc'd, NUL-terminated UTF-8 string (caller must free()), or NULL on
 * error. English-only ("en") language is fixed for now. */
char *stt_transcribe(struct stt_context *stt, const float *samples, size_t n_samples);

void stt_free(struct stt_context *stt);

#endif
