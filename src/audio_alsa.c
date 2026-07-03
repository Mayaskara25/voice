#include "audio_alsa.h"
#include "log.h"

#include <alsa/asoundlib.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ALSA capture is done via S16_LE, not FLOAT_LE: this is the "safe/most
 * compatible" choice noted in PLAN.md (converted to normalized float here,
 * not smeared into stt_whisper.c) since not every device/plugin chain in the
 * PipeWire-ALSA-compat stack is guaranteed to support FLOAT_LE capture. */
#define CHANNELS      1
#define CHUNK_FRAMES  1600  /* 100ms at 16kHz */
#define LATENCY_US    100000

/* PipeWire's "default" ALSA node was found (empirically, see PLAN.md) to
 * corrupt/clip 16kHz mono capture on this machine's hardware, while ALSA's
 * own "plughw:<card>,0" conversion path (bypassing PipeWire's routing
 * entirely) produces clean audio. Rather than hardcoding a card number
 * (fragile across machines), find the first card that exposes a capture
 * device via ALSA's control API. Falls back to "default" if none is found. */
static bool find_capture_device(char *out, size_t out_size)
{
    int card = -1;
    if (snd_card_next(&card) < 0 || card < 0)
        return false;

    while (card >= 0) {
        char ctlname[16];
        snprintf(ctlname, sizeof(ctlname), "hw:%d", card);

        snd_ctl_t *ctl;
        if (snd_ctl_open(&ctl, ctlname, 0) == 0) {
            int dev = -1;
            snd_ctl_pcm_next_device(ctl, &dev);
            while (dev >= 0) {
                snd_pcm_info_t *info;
                snd_pcm_info_alloca(&info);
                snd_pcm_info_set_device(info, dev);
                snd_pcm_info_set_subdevice(info, 0);
                snd_pcm_info_set_stream(info, SND_PCM_STREAM_CAPTURE);

                if (snd_ctl_pcm_info(ctl, info) == 0) {
                    snprintf(out, out_size, "plughw:%d,%d", card, dev);
                    snd_ctl_close(ctl);
                    return true;
                }
                snd_ctl_pcm_next_device(ctl, &dev);
            }
            snd_ctl_close(ctl);
        }
        snd_card_next(&card);
    }
    return false;
}

struct capture_state {
    pthread_t thread;
    pthread_mutex_t mutex;
    bool running;
    volatile bool stop_requested;
    snd_pcm_t *pcm;
    float *buf;
    size_t len;
    size_t cap;
};

static struct capture_state g_cap;

static void buf_append(struct capture_state *cs, const int16_t *frames, size_t n)
{
    if (cs->len + n > cs->cap) {
        size_t newcap = cs->cap == 0 ? (size_t)AUDIO_SAMPLE_RATE : cs->cap * 2;
        while (newcap < cs->len + n)
            newcap *= 2;
        float *nb = realloc(cs->buf, newcap * sizeof(float));
        if (!nb) {
            log_error("audio: out of memory growing capture buffer");
            return;
        }
        cs->buf = nb;
        cs->cap = newcap;
    }
    for (size_t i = 0; i < n; i++)
        cs->buf[cs->len + i] = frames[i] / 32768.0f;
    cs->len += n;
}

static void *capture_thread_fn(void *arg)
{
    struct capture_state *cs = arg;
    int16_t chunk[CHUNK_FRAMES];

    while (!cs->stop_requested) {
        snd_pcm_sframes_t r = snd_pcm_readi(cs->pcm, chunk, CHUNK_FRAMES);
        if (r == -EPIPE) {
            log_warn("audio: capture overrun, recovering");
            snd_pcm_prepare(cs->pcm);
            continue;
        } else if (r == -EAGAIN) {
            continue;
        } else if (r < 0) {
            log_error("audio: read error: %s", snd_strerror((int)r));
            break;
        }
        pthread_mutex_lock(&cs->mutex);
        buf_append(cs, chunk, (size_t)r);
        pthread_mutex_unlock(&cs->mutex);
    }
    return NULL;
}

int audio_capture_start(const char *device_override)
{
    memset(&g_cap, 0, sizeof(g_cap));
    pthread_mutex_init(&g_cap.mutex, NULL);

    char device[32];
    if (device_override && device_override[0] != '\0') {
        snprintf(device, sizeof(device), "%s", device_override);
    } else if (!find_capture_device(device, sizeof(device))) {
        log_warn("audio: no ALSA capture card found, falling back to 'default'");
        snprintf(device, sizeof(device), "default");
    }

    int err = snd_pcm_open(&g_cap.pcm, device, SND_PCM_STREAM_CAPTURE, 0);
    if (err < 0) {
        log_error("audio: cannot open capture device '%s': %s", device, snd_strerror(err));
        return -1;
    }
    log_info("audio: using capture device '%s'", device);

    err = snd_pcm_set_params(g_cap.pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                              CHANNELS, AUDIO_SAMPLE_RATE, 1 /* allow resample */, LATENCY_US);
    if (err < 0) {
        log_error("audio: cannot set stream params: %s", snd_strerror(err));
        snd_pcm_close(g_cap.pcm);
        return -1;
    }

    g_cap.stop_requested = false;
    if (pthread_create(&g_cap.thread, NULL, capture_thread_fn, &g_cap) != 0) {
        log_error("audio: failed to spawn capture thread");
        snd_pcm_close(g_cap.pcm);
        return -1;
    }
    g_cap.running = true;

    log_info("audio: capture started");
    return 0;
}

int audio_capture_stop(struct audio_buffer *out)
{
    out->samples = NULL;
    out->n_samples = 0;

    if (!g_cap.running)
        return -1;

    g_cap.stop_requested = true;
    pthread_join(g_cap.thread, NULL);
    snd_pcm_close(g_cap.pcm);
    g_cap.running = false;

    pthread_mutex_lock(&g_cap.mutex);
    out->samples = g_cap.buf;
    out->n_samples = g_cap.len;
    g_cap.buf = NULL;
    g_cap.len = 0;
    g_cap.cap = 0;
    pthread_mutex_unlock(&g_cap.mutex);
    pthread_mutex_destroy(&g_cap.mutex);

    log_info("audio: capture stopped, %zu samples (%.2fs)",
              out->n_samples, (double)out->n_samples / AUDIO_SAMPLE_RATE);
    return 0;
}

void audio_capture_free(struct audio_buffer *buf)
{
    free(buf->samples);
    buf->samples = NULL;
    buf->n_samples = 0;
}
