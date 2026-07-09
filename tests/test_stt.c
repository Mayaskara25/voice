/* test_stt: the Phase A whisper validation, made repeatable -- decode the
 * vendored jfk.wav and assert the known transcript appears. Skips (exit 0) if
 * the model file is absent, so the suite still runs on a fresh checkout. */
#include "stt_whisper.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define MODEL_PATH "models/ggml-base.en.bin"
#define WAV_PATH   "whisper.cpp/samples/jfk.wav"

/* Minimal 16-bit PCM WAV loader -> normalized float mono. Returns samples
 * (caller frees) and count via n_out, or NULL on error. */
static float *load_wav(const char *path, size_t *n_out)
{
    *n_out = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 44) { fclose(f); return NULL; }

    unsigned char *buf = malloc((size_t)sz);
    if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); return NULL; }
    fclose(f);

    int channels = 1, bits = 16;
    float *out = NULL;
    long i = 12; /* skip "RIFF"<size>"WAVE" */
    while (i + 8 <= sz) {
        unsigned char *id = buf + i;
        uint32_t clen = buf[i+4] | (buf[i+5] << 8) | (buf[i+6] << 16) | ((uint32_t)buf[i+7] << 24);
        long body = i + 8;
        if (memcmp(id, "fmt ", 4) == 0 && body + 16 <= sz) {
            channels = buf[body+2] | (buf[body+3] << 8);
            bits     = buf[body+14] | (buf[body+15] << 8);
        } else if (memcmp(id, "data", 4) == 0) {
            if (bits != 16 || channels < 1) break;
            size_t nsamp = clen / 2;
            out = malloc(nsamp * sizeof(float));
            if (!out) break;
            size_t k = 0;
            for (size_t s = 0; s + (size_t)channels <= nsamp; s += (size_t)channels) {
                int16_t v;
                memcpy(&v, buf + body + s * 2, sizeof(v)); /* first channel; unaligned-safe */
                out[k++] = v / 32768.0f;
            }
            *n_out = k;
            break;
        }
        i = body + clen + (clen & 1);
    }
    free(buf);
    return out;
}

int main(void)
{
    struct stat st;
    if (stat(MODEL_PATH, &st) != 0) {
        printf("test_stt: SKIP (model '%s' not present -- run download-ggml-model.sh)\n", MODEL_PATH);
        return 0;
    }

    size_t n = 0;
    float *samples = load_wav(WAV_PATH, &n);
    if (!samples || n == 0) {
        fprintf(stderr, "test_stt: FAIL to load '%s'\n", WAV_PATH);
        free(samples);
        return 1;
    }

    struct stt_context stt;
    /* exercise the GPU path (env override: DICT_TEST_GPU=0 forces CPU) */
    bool use_gpu = true;
    const char *g = getenv("DICT_TEST_GPU");
    if (g && atoi(g) == 0) use_gpu = false;
    if (stt_init(&stt, MODEL_PATH, 4, use_gpu, NULL) != 0) {
        fprintf(stderr, "test_stt: FAIL stt_init\n");
        free(samples);
        return 1;
    }

    char *text = stt_transcribe(&stt, samples, n);
    stt_free(&stt);
    free(samples);

    if (!text) { fprintf(stderr, "test_stt: FAIL stt_transcribe returned NULL\n"); return 1; }

    for (char *p = text; *p; p++) *p = (char)tolower((unsigned char)*p);
    int ok = strstr(text, "country") != NULL;
    if (!ok)
        fprintf(stderr, "test_stt: FAIL -- expected 'country' in transcript, got: %s\n", text);
    free(text);

    if (!ok) return 1;
    printf("test_stt: OK\n");
    return 0;
}
