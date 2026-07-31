/* dictation-setup: model picker / downloader for the dictation daemon.
 *
 * Opens the microui/Xlib setup window (D3) by default; `--list` and
 * `--fetch-model` (D2) stay as headless equivalents, which is how the
 * fork/exec/pipe/CR-translation work was exercised from a terminal before any
 * window existed. (Same discipline Phase A used to validate stt_whisper.c
 * standalone.) They remain useful over ssh and for scripting.
 *
 * Deliberately a separate binary from `dictation`: scripts/waybar-dictation.sh
 * identifies the daemon by executable name (`pgrep -x dictation`), so a setup
 * window running as `dictation --setup` would be indistinguishable from a
 * running daemon. See PLAN.md, Phase D.
 *
 * Links neither whisper, llama nor CUDA -- on a fresh clone this builds in
 * seconds and can fetch the models while the real dependency build runs. */
#include "config_write.h"
#include "downloader.h"
#include "log.h"
#include "model_catalog.h"
#include "setup_gui.h"

#include <X11/Xlib.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_CATALOG    "configs/models.conf"
#define DEFAULT_MODELS_DIR "./models"

static volatile sig_atomic_t g_interrupted = 0;

static void on_sigint(int sig)
{
    (void)sig;
    g_interrupted = 1;
    setup_gui_request_stop();   /* no-op unless the window is running */
}

static void print_usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s [--catalog PATH] [--models-dir DIR] [--list] [--fetch-model ID]\n"
        "  (no arguments)     open the setup window\n"
        "  --catalog PATH     model catalog (default: %s)\n"
        "  --models-dir DIR   where models are stored (default: %s)\n"
        "  --font XLFD        X core font for the window (default: 9x15)\n"
        "  --dir PATH         project directory to work in (default: the one holding\n"
        "                     this binary, if it has a scripts/ directory)\n"
        "  --list             print the catalog with present/missing status, then exit\n"
        "  --fetch-model ID   download that catalog entry, then exit\n"
        "  -h, --help         this message\n"
        "\n"
        "Relative paths resolve against the project directory, which this binary\n"
        "changes into at startup (see --dir) -- the same cd the launcher script does\n"
        "before starting the daemon.\n",
        argv0, DEFAULT_CATALOG, DEFAULT_MODELS_DIR);
}

static void print_catalog(const struct model_catalog *c)
{
    for (size_t i = 0; i < c->n; i++) {
        const struct model_entry *m = &c->e[i];
        printf("%-8s %-24s %-9s %s\n"
               "         %s\n",
               m->kind, m->id, m->present ? "ready" : "MISSING", m->display, m->path);
    }
    if (c->n == 0)
        printf("(catalog is empty)\n");
}

/* The same fraction D3's progress bar will draw, printed as a line so the
 * stat()-based progress can be seen working before any bar exists. */
static void print_progress(const struct download *d)
{
    if (d->expected_bytes > 0) {
        double pct = 100.0 * (double)d->got_bytes / (double)d->expected_bytes;
        printf("progress: %5.1f%%  %ld/%ld bytes\n", pct, d->got_bytes, d->expected_bytes);
    } else {
        printf("progress: %ld bytes (total size unknown)\n", d->got_bytes);
    }
    fflush(stdout);
}

static int fetch_model(const struct model_catalog *c, const char *id)
{
    const struct model_entry *m = catalog_find(c, id);
    if (!m) {
        log_error("setup: no catalog entry with id '%s' -- run --list to see the ids", id);
        return 1;
    }
    if (m->present) {
        log_info("setup: %s is already at %s -- nothing to do", m->id, m->path);
        return 0;
    }
    if (download_check_curl() != 0)
        return 1;

    struct download d;
    if (download_start(&d, m->url, m->path, m->size) != 0)
        return 1;

    /* First line of the output is the exact command, so a failure can be
     * reproduced by pasting it into a terminal. */
    printf("$ %s\n", d.cmdline);
    fflush(stdout);

    struct timespec last_progress = { 0, 0 };
    int finished = 0;

    while (!g_interrupted && !finished) {
        if (d.out_fd >= 0) {
            struct pollfd p = { .fd = d.out_fd, .events = POLLIN, .revents = 0 };
            int r = poll(&p, 1, 250);
            if (r < 0 && errno != EINTR) {
                log_error("setup: poll failed: %s", strerror(errno));
                break;
            }
            /* Read on ANY event, not just POLLIN: when curl exits, poll reports
             * POLLHUP with no POLLIN, and a POLLIN-only loop would spin at 100%
             * CPU forever instead of noticing the end of output. */
            if (r > 0 && p.revents != 0) {
                char buf[4096];
                int got;
                while ((got = download_read(&d, buf, sizeof(buf))) > 0) {
                    fwrite(buf, 1, (size_t)got, stdout);
                    fflush(stdout);
                }
            }
        } else {
            /* Output is done but the child hasn't been reaped yet; nothing to
             * poll on, so tick slowly rather than spinning. */
            nanosleep(&(struct timespec){ .tv_nsec = 10 * 1000 * 1000 }, NULL);
        }

        download_update_progress(&d);
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        /* at_line_start: don't cut into a half-written curl line. Its meter
         * arrives in fragments, so printing on a bare timer splices our line
         * into the middle of one of its words. */
        if (now.tv_sec != last_progress.tv_sec && d.at_line_start) {
            print_progress(&d);
            last_progress = now;
        }

        int r = download_reap(&d);
        if (r != 0) {
            /* Drain what curl wrote between the last read and its exit. That
             * data is still sitting in the pipe, and on a fast transfer it is
             * the entire error message -- the thing this whole design exists to
             * put on screen. Loop to EOF (-1), not to the first 0 (EAGAIN). */
            char buf[4096];
            while (d.out_fd >= 0) {
                int got = download_read(&d, buf, sizeof(buf));
                if (got < 0)
                    break;
                if (got > 0) {
                    fwrite(buf, 1, (size_t)got, stdout);
                    fflush(stdout);
                }
            }
            finished = 1;
        }
    }

    if (!finished) {
        printf("\n");
        log_warn("setup: interrupted -- cancelling");
        download_cancel(&d);
        return 130;
    }

    if (d.exit_code != 0) {
        log_error("setup: curl exited %d -- the full output is above, and the command can be "
                  "re-run verbatim: %s", d.exit_code, d.cmdline);
        return 1;
    }
    print_progress(&d);
    return 0;
}

/* Every path this tool touches -- the catalog, models/, configs/local.conf, the
 * launcher script -- is relative to the project directory, exactly as the
 * daemon's are (scripts/waybar-dictation.sh cd's there before exec'ing it). But
 * a .desktop launcher (D5) starts us with cwd=$HOME, which would put
 * configs/local.conf in the wrong place entirely. So resolve the project
 * directory from /proc/self/exe and chdir there.
 *
 * Only when it really looks like the project, though: an installed copy at
 * /usr/local/bin has no scripts/ next to it, and chdir'ing there would be
 * worse than doing nothing. Writes the resolved directory into `out`. */
static int resolve_project_dir(char *out, size_t n)
{
    char exe[CONFIG_PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (len <= 0)
        return -1;
    exe[len] = '\0';

    char *slash = strrchr(exe, '/');
    if (!slash)
        return -1;
    *slash = '\0';

    char probe[CONFIG_PATH_MAX];
    int w = snprintf(probe, sizeof(probe), "%s/scripts/waybar-dictation.sh", exe);
    if (w < 0 || (size_t)w >= sizeof(probe) || access(probe, F_OK) != 0)
        return -1;

    w = snprintf(out, n, "%s", exe);
    return (w < 0 || (size_t)w >= n) ? -1 : 0;
}

/* Opens the window. Separate from main() so the catalog teardown below stays in
 * one place. Returns the process exit code. */
static int run_window(struct model_catalog *catalog, const char *models_dir, const char *font,
                      const char *project_dir)
{
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        log_error("setup: cannot open an X display (DISPLAY=%s) -- use --list and "
                  "--fetch-model for a headless setup",
                  getenv("DISPLAY") ? getenv("DISPLAY") : "unset");
        return 1;
    }

    struct setup_gui gui;
    if (setup_gui_init(&gui, dpy, catalog, models_dir, font, project_dir) != 0) {
        XCloseDisplay(dpy);
        return 1;
    }

    setup_gui_run(&gui);
    setup_gui_destroy(&gui);
    XCloseDisplay(dpy);
    return 0;
}

int main(int argc, char **argv)
{
    const char *catalog_path = DEFAULT_CATALOG;
    const char *models_dir = DEFAULT_MODELS_DIR;
    const char *fetch_id = NULL;
    const char *font = NULL;
    const char *dir_override = NULL;
    bool list_only = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--catalog") == 0 && i + 1 < argc) {
            catalog_path = argv[++i];
        } else if (strcmp(argv[i], "--models-dir") == 0 && i + 1 < argc) {
            models_dir = argv[++i];
        } else if (strcmp(argv[i], "--fetch-model") == 0 && i + 1 < argc) {
            fetch_id = argv[++i];
        } else if (strcmp(argv[i], "--font") == 0 && i + 1 < argc) {
            font = argv[++i];
        } else if (strcmp(argv[i], "--dir") == 0 && i + 1 < argc) {
            dir_override = argv[++i];
        } else if (strcmp(argv[i], "--list") == 0) {
            list_only = true;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "unknown argument: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    /* The parent never writes to curl's pipe, but a stray SIGPIPE would kill
     * the process outright; ignoring it here (in main, not in the library) keeps
     * the disposition change where it's visible. */
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, on_sigint);

    /* chdir before anything reads a relative path. */
    char project_dir[CONFIG_PATH_MAX];
    if (dir_override) {
        snprintf(project_dir, sizeof(project_dir), "%s", dir_override);
    } else if (resolve_project_dir(project_dir, sizeof(project_dir)) != 0) {
        /* Not next to a scripts/ directory: stay where we are and let the
         * relative defaults resolve against the current directory, the way the
         * D2 CLI always has. */
        project_dir[0] = '\0';
    }
    if (project_dir[0] != '\0') {
        if (chdir(project_dir) != 0) {
            log_error("setup: cannot chdir to '%s': %s", project_dir, strerror(errno));
            return 1;
        }
        log_info("setup: working directory %s", project_dir);
    } else if (!getcwd(project_dir, sizeof(project_dir))) {
        project_dir[0] = '\0';
    }

    struct model_catalog catalog;
    if (catalog_load(&catalog, catalog_path) != 0)
        return 1;
    catalog_refresh_presence(&catalog, models_dir);

    int rc;
    if (fetch_id) {
        rc = fetch_model(&catalog, fetch_id);
    } else if (list_only) {
        print_catalog(&catalog);
        printf("\nconfig: %s\n", config_default_path());
        printf("fetch a missing model with: %s --fetch-model <id>\n", argv[0]);
        rc = 0;
    } else {
        rc = run_window(&catalog, models_dir, font, project_dir);
    }

    catalog_free(&catalog);
    return rc;
}
