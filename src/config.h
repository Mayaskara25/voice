#ifndef DICTATION_CONFIG_H
#define DICTATION_CONFIG_H

#include <stdbool.h>

#define CONFIG_PATH_MAX 512

struct app_config {
    char whisper_model_path[CONFIG_PATH_MAX];
    char llama_model_path[CONFIG_PATH_MAX];  /* empty = LLM cleanup disabled (Phase B) */
    char ptt_device[CONFIG_PATH_MAX];        /* empty = auto-detect */
    int  ptt_keycode;                        /* Linux evdev code (input-event-codes.h), NOT an X keysym/keycode */
    char audio_device[CONFIG_PATH_MAX];       /* empty = auto-detect an ALSA capture card; e.g. "plughw:2,0" */
    int  n_threads;
    bool whisper_use_gpu;                    /* true: offload whisper to the GPU (else CPU) */
    char language[8];
    char whisper_initial_prompt[256];        /* biases whisper toward this vocabulary/phrasing; empty = none */
    bool test_mode;                          /* true: print transcript instead of injecting */
    bool gui_enabled;                        /* true: also run the microui/Xlib status panel (Phase C) */
    char gui_font[CONFIG_PATH_MAX];          /* X core font (XLFD/alias) for the GUI; empty = "fixed" */
    char cleanup_style[32];                  /* LLM cleanup style: dictation|code|commands (Phase B) */
    int  n_gpu_layers;                       /* llama GPU offload: 0 = CPU, 99 = all layers (Phase B) */
};

/* Populates *cfg with defaults, then overrides from the key=value file at path.
 * Returns 0 on success, -1 on error (file not found/unreadable) with a message logged. */
int config_load(const char *path, struct app_config *cfg);

/* Returns 0 if cfg is usable (referenced model files exist etc.), -1 otherwise
 * with a descriptive message logged. */
int config_validate(const struct app_config *cfg);

void config_set_defaults(struct app_config *cfg);

#endif
