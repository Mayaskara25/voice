#include "stt_whisper.h"
#include "log.h"

#include <whisper.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int stt_init(struct stt_context *stt, const char *model_path, int n_threads, bool use_gpu,
             const char *initial_prompt)
{
    memset(stt, 0, sizeof(*stt));
    if (initial_prompt)
        snprintf(stt->initial_prompt, sizeof(stt->initial_prompt), "%s", initial_prompt);

    struct whisper_context_params cparams = whisper_context_default_params();
    /* GPU offload via the shared ggml CUDA backend (whisper falls back to CPU
     * on its own if no GPU device is found). */
    cparams.use_gpu    = use_gpu;
    cparams.gpu_device = 0;

    stt->whisper_ctx = whisper_init_from_file_with_params(model_path, cparams);
    if (!stt->whisper_ctx) {
        log_error("stt: failed to load whisper model '%s'", model_path);
        return -1;
    }

    stt->n_threads = n_threads > 0 ? n_threads : 4;
    log_info("stt: whisper model loaded from '%s' (n_threads=%d, use_gpu=%s)",
             model_path, stt->n_threads, use_gpu ? "true" : "false");
    return 0;
}

char *stt_transcribe(struct stt_context *stt, const float *samples, size_t n_samples)
{
    if (!stt->whisper_ctx) {
        log_error("stt: stt_transcribe called before stt_init");
        return NULL;
    }
    if (n_samples == 0) {
        log_warn("stt: empty audio buffer, nothing to transcribe");
        return NULL;
    }

    struct whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    wparams.n_threads       = stt->n_threads;
    wparams.language        = "en"; /* English-only per PLAN.md */
    wparams.translate       = false;
    wparams.print_progress  = false;
    wparams.print_realtime  = false;
    wparams.print_special   = false;
    wparams.print_timestamps = false;
    wparams.no_context      = true; /* each PTT clip is transcribed independently */
    wparams.initial_prompt  = stt->initial_prompt[0] ? stt->initial_prompt : NULL;

    int rc = whisper_full(stt->whisper_ctx, wparams, samples, (int)n_samples);
    if (rc != 0) {
        log_error("stt: whisper_full failed (rc=%d)", rc);
        return NULL;
    }

    int n_segments = whisper_full_n_segments(stt->whisper_ctx);
    size_t cap = 256;
    size_t len = 0;
    char *out = malloc(cap);
    if (!out) {
        log_error("stt: out of memory");
        return NULL;
    }
    out[0] = '\0';

    for (int i = 0; i < n_segments; i++) {
        const char *seg = whisper_full_get_segment_text(stt->whisper_ctx, i);
        size_t seg_len = strlen(seg);
        if (len + seg_len + 1 > cap) {
            size_t newcap = cap * 2;
            while (newcap < len + seg_len + 1)
                newcap *= 2;
            char *nb = realloc(out, newcap);
            if (!nb) {
                log_error("stt: out of memory concatenating segments");
                free(out);
                return NULL;
            }
            out = nb;
            cap = newcap;
        }
        memcpy(out + len, seg, seg_len);
        len += seg_len;
        out[len] = '\0';
    }

    return out;
}

void stt_free(struct stt_context *stt)
{
    if (stt->whisper_ctx) {
        whisper_free(stt->whisper_ctx);
        stt->whisper_ctx = NULL;
    }
}
