#include "audio_alsa.h"
#include "config.h"
#include "hotkey_evdev.h"
#include "inject_xtest.h"
#include "log.h"
#include "stt_whisper.h"

#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct pipeline_ctx {
    struct stt_context *stt;
    Display *dpy;               /* NULL in test_mode */
    const struct app_config *cfg;
};

static void on_ptt_down(void *user_data)
{
    struct pipeline_ctx *ctx = user_data;
    log_info("pipeline: recording...");
    audio_capture_start(ctx->cfg->audio_device);
}

static void on_ptt_up(void *user_data)
{
    struct pipeline_ctx *ctx = user_data;

    struct audio_buffer buf;
    if (audio_capture_stop(&buf) != 0 || buf.n_samples == 0) {
        log_warn("pipeline: no audio captured");
        return;
    }

    log_info("pipeline: transcribing...");
    char *text = stt_transcribe(ctx->stt, buf.samples, buf.n_samples);
    audio_capture_free(&buf);

    if (!text || text[0] == '\0') {
        log_warn("pipeline: empty transcript, nothing to do");
        free(text);
        return;
    }

    if (ctx->cfg->test_mode) {
        printf("%s\n", text);
        fflush(stdout);
    } else {
        log_info("pipeline: injecting text");
        inject_type_text(ctx->dpy, text);
    }

    free(text);
}

struct hotkey_thread_args {
    const struct app_config *cfg;
    void *user_data;
};

static void *hotkey_thread_fn(void *arg)
{
    struct hotkey_thread_args *args = arg;
    hotkey_run(args->cfg->ptt_device, args->cfg->ptt_keycode,
               on_ptt_down, on_ptt_up, args->user_data);
    return NULL;
}

static void handle_signal(int sig)
{
    (void)sig;
    hotkey_request_stop();
}

static void print_usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s [--config PATH] [--test-mode] [--list-keys] [-h|--help]\n"
        "  --config PATH   config file (default: configs/example.conf)\n"
        "  --test-mode     print transcripts to stdout instead of injecting\n"
        "  --list-keys     print (device, evdev code, name) for keypresses, then exit\n",
        argv0);
}

int main(int argc, char **argv)
{
    const char *config_path = "configs/example.conf";
    bool force_test_mode = false;
    bool list_keys = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            config_path = argv[++i];
        } else if (strcmp(argv[i], "--test-mode") == 0) {
            force_test_mode = true;
        } else if (strcmp(argv[i], "--list-keys") == 0) {
            list_keys = true;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "unknown argument: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    if (list_keys)
        return hotkey_list_keys() == 0 ? 0 : 1;

    struct app_config cfg;
    if (config_load(config_path, &cfg) != 0)
        return 1;
    if (force_test_mode)
        cfg.test_mode = true;
    if (config_validate(&cfg) != 0)
        return 1;

    struct stt_context stt;
    if (stt_init(&stt, cfg.whisper_model_path, cfg.n_threads) != 0)
        return 1;

    Display *dpy = NULL;
    if (!cfg.test_mode) {
        dpy = inject_open_display();
        if (!dpy) {
            stt_free(&stt);
            return 1;
        }
    }

    struct pipeline_ctx pctx = { .stt = &stt, .dpy = dpy, .cfg = &cfg };
    struct hotkey_thread_args hargs = { .cfg = &cfg, .user_data = &pctx };

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    log_info("dictation: ready (test_mode=%s, gui=%s) -- hold your PTT key to dictate",
              cfg.test_mode ? "true" : "false", cfg.gui_enabled ? "true" : "false");

    pthread_t hotkey_thread;
    if (pthread_create(&hotkey_thread, NULL, hotkey_thread_fn, &hargs) != 0) {
        log_error("main: failed to spawn hotkey thread");
        stt_free(&stt);
        inject_close_display(dpy);
        return 1;
    }

    pthread_join(hotkey_thread, NULL);

    log_info("dictation: shutting down");
    stt_free(&stt);
    inject_close_display(dpy);
    return 0;
}
