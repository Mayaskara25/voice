#include "audio_alsa.h"
#include "config.h"
#include "gui_xlib.h"
#include "hotkey_evdev.h"
#include "inject_xtest.h"
#include "ipc_handoff.h"
#include "llm_cleanup.h"
#include "log.h"
#include "stt_whisper.h"

#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct pipeline_ctx {
    struct stt_context      *stt;
    struct llm_context      *llm;   /* NULL = LLM cleanup disabled */
    Display                 *dpy;   /* headless direct-injection target; NULL in test-mode/GUI mode */
    const struct app_config *cfg;
    struct ipc_handoff      *ipc;   /* non-NULL in GUI mode: publish state + hand off injection */
};

/* Publishes a pipeline state to the GUI (no-op in headless mode). */
static void publish_state(struct pipeline_ctx *ctx, enum app_state st)
{
    if (ctx->ipc)
        ipc_set_state(ctx->ipc, st);
}

static void on_ptt_down(void *user_data)
{
    struct pipeline_ctx *ctx = user_data;
    log_info("pipeline: recording...");
    publish_state(ctx, APP_STATE_RECORDING);
    audio_capture_start(ctx->cfg->audio_device);
}

static void on_ptt_up(void *user_data)
{
    struct pipeline_ctx *ctx = user_data;

    struct audio_buffer buf;
    if (audio_capture_stop(&buf) != 0 || buf.n_samples == 0) {
        log_warn("pipeline: no audio captured");
        publish_state(ctx, APP_STATE_IDLE);
        return;
    }

    log_info("pipeline: transcribing...");
    publish_state(ctx, APP_STATE_TRANSCRIBING);
    char *text = stt_transcribe(ctx->stt, buf.samples, buf.n_samples);
    audio_capture_free(&buf);

    if (!text || text[0] == '\0') {
        log_warn("pipeline: empty transcript, nothing to do");
        free(text);
        publish_state(ctx, APP_STATE_IDLE);
        return;
    }

    /* Optional LLM cleanup (Phase B). Best-effort: on any failure llm_clean
     * returns NULL and we keep the raw transcript. GUI already renders CLEANING. */
    if (ctx->llm) {
        log_info("pipeline: cleaning up transcript (style=%s)...", ctx->cfg->cleanup_style);
        publish_state(ctx, APP_STATE_CLEANING);
        char *cleaned = llm_clean(ctx->llm, text, ctx->cfg->cleanup_style);
        if (cleaned) {
            free(text);
            text = cleaned;
        } else {
            log_warn("pipeline: cleanup failed, using raw transcript");
        }
    }

    if (ctx->cfg->test_mode) {
        printf("%s\n", text);
        fflush(stdout);
        free(text);
        publish_state(ctx, APP_STATE_IDLE);
    } else if (ctx->ipc) {
        /* GUI mode: hand the text (ownership transfers) to the GUI thread, which
         * owns the X Display, performs the injection, then sets IDLE. This keeps
         * Display single-threaded (see PLAN.md forward-compatibility contract). */
        log_info("pipeline: handing text to GUI thread for injection");
        ipc_post_inject(ctx->ipc, text);
    } else {
        /* Headless mode: inject directly on this (worker) thread. */
        log_info("pipeline: injecting text");
        inject_type_text(ctx->dpy, text);
        free(text);
    }
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
    gui_request_stop(); /* no-op if the GUI isn't running */
}

static void print_usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s [--config PATH] [--test-mode] [--gui] [--list-keys] [-h|--help]\n"
        "  --config PATH   config file (default: configs/example.conf)\n"
        "  --test-mode     print transcripts to stdout instead of injecting\n"
        "  --gui           show the microui/Xlib status panel (overrides config)\n"
        "  --list-keys     print (device, evdev code, name) for keypresses, then exit\n",
        argv0);
}

/* Runs the headless pipeline: worker thread injects directly, main joins.
 * Identical behavior to Phase A. Returns process exit code. */
static int run_headless(struct stt_context *stt, struct llm_context *llm, struct app_config *cfg)
{
    Display *dpy = NULL;
    if (!cfg->test_mode) {
        dpy = inject_open_display();
        if (!dpy)
            return 1;
    }

    struct pipeline_ctx pctx = { .stt = stt, .llm = llm, .dpy = dpy, .cfg = cfg, .ipc = NULL };
    struct hotkey_thread_args hargs = { .cfg = cfg, .user_data = &pctx };

    log_info("dictation: ready (test_mode=%s, gui=false) -- hold your PTT key to dictate",
             cfg->test_mode ? "true" : "false");

    pthread_t hotkey_thread;
    if (pthread_create(&hotkey_thread, NULL, hotkey_thread_fn, &hargs) != 0) {
        log_error("main: failed to spawn hotkey thread");
        inject_close_display(dpy);
        return 1;
    }

    pthread_join(hotkey_thread, NULL);
    inject_close_display(dpy);
    return 0;
}

/* Runs the GUI pipeline: main thread becomes the X/GUI thread (owns Display,
 * renders, injects); worker thread runs the same capture->stt pipeline but
 * publishes state and hands off injection via ipc_handoff. Returns exit code. */
static int run_gui(struct stt_context *stt, struct llm_context *llm, struct app_config *cfg)
{
    Display *dpy = inject_open_display();
    if (!dpy)
        return 1;

    struct ipc_handoff ipc;
    if (ipc_init(&ipc) != 0) {
        inject_close_display(dpy);
        return 1;
    }

    struct gui gui;
    if (gui_init(&gui, dpy, &ipc, cfg->gui_font) != 0) {
        ipc_free(&ipc);
        inject_close_display(dpy);
        return 1;
    }

    struct pipeline_ctx pctx = { .stt = stt, .llm = llm, .dpy = NULL, .cfg = cfg, .ipc = &ipc };
    struct hotkey_thread_args hargs = { .cfg = cfg, .user_data = &pctx };

    log_info("dictation: ready (test_mode=%s, gui=true) -- hold your PTT key to dictate",
             cfg->test_mode ? "true" : "false");

    pthread_t hotkey_thread;
    if (pthread_create(&hotkey_thread, NULL, hotkey_thread_fn, &hargs) != 0) {
        log_error("main: failed to spawn hotkey thread");
        gui_destroy(&gui);
        ipc_free(&ipc);
        inject_close_display(dpy);
        return 1;
    }

    gui_run(&gui);            /* blocks on the main/GUI thread until signaled */
    hotkey_request_stop();    /* make sure the worker unwinds too */
    pthread_join(hotkey_thread, NULL);

    gui_destroy(&gui);
    ipc_free(&ipc);
    inject_close_display(dpy);
    return 0;
}

int main(int argc, char **argv)
{
    const char *config_path = "configs/example.conf";
    bool force_test_mode = false;
    bool force_gui = false;
    bool list_keys = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            config_path = argv[++i];
        } else if (strcmp(argv[i], "--test-mode") == 0) {
            force_test_mode = true;
        } else if (strcmp(argv[i], "--gui") == 0) {
            force_gui = true;
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
    if (force_gui)
        cfg.gui_enabled = true;
    if (config_validate(&cfg) != 0)
        return 1;

    struct stt_context stt;
    if (stt_init(&stt, cfg.whisper_model_path, cfg.n_threads) != 0)
        return 1;

    /* Optional LLM cleanup (Phase B): only when a model is configured. Failure
     * to load is fatal here (misconfiguration) rather than silently skipping. */
    struct llm_context llm;
    struct llm_context *llm_ptr = NULL;
    if (cfg.llama_model_path[0] != '\0') {
        if (llm_init(&llm, cfg.llama_model_path, cfg.n_threads, cfg.n_gpu_layers) != 0) {
            stt_free(&stt);
            return 1;
        }
        llm_ptr = &llm;
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    int ret = cfg.gui_enabled ? run_gui(&stt, llm_ptr, &cfg)
                              : run_headless(&stt, llm_ptr, &cfg);

    log_info("dictation: shutting down");
    if (llm_ptr)
        llm_free(llm_ptr);
    stt_free(&stt);
    return ret;
}
