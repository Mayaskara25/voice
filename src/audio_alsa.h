#ifndef DICTATION_AUDIO_ALSA_H
#define DICTATION_AUDIO_ALSA_H

#include <stddef.h>

#define AUDIO_SAMPLE_RATE 16000 /* must match WHISPER_SAMPLE_RATE */

struct audio_buffer {
    float *samples;   /* normalized [-1, 1], mono, 16kHz */
    size_t n_samples;
};

/* Opens an ALSA capture device and starts a background thread appending
 * audio into a growing buffer. If device is NULL or empty, auto-detects the
 * first capture-capable ALSA card (see PLAN.md for why "default" is
 * deliberately avoided). Returns 0 on success, -1 on error. */
int audio_capture_start(const char *device);

/* Stops the capture thread, closes the device, and hands back everything
 * captured since audio_capture_start(). Caller must audio_capture_free() the
 * result. Returns 0 on success, -1 if no capture was in progress. */
int audio_capture_stop(struct audio_buffer *out);

void audio_capture_free(struct audio_buffer *buf);

#endif
