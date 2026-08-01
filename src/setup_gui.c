/* pipe2(): a GNU extension, used so the status pipe is O_CLOEXEC atomically. */
#define _GNU_SOURCE

#include "setup_gui.h"
#include "config.h"
#include "config_write.h"
#include "log.h"
#include "xclip.h"

#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define SETUP_WIDTH   560
#define SETUP_HEIGHT  640
#define SETUP_MIN_W   420
#define SETUP_MIN_H   380
/* 'fixed' is 13px -- cramped for a widget-dense form. Not always installed:
 * on Arch it comes from xorg-fonts-misc, and font_open falls back to 'fixed'
 * (which every X server has) when it is missing. */
#define SETUP_FONT    "9x15"

/* The microui container is keyed by this string, so it must be identical in
 * mu_get_container and mu_begin_window_ex. */
#define SETUP_TITLE   "dictation setup"

/* Same contract as SETUP_TITLE: the container key for the download prompt. */
#define SETUP_PROMPT   "download prompt"
/* Roomy enough for a typical absolute model path, but width alone cannot be
 * relied on -- 78 characters is 468px in the 6px fallback font and 702px in the
 * 9x15 this window actually asks for, and paths have no upper bound anyway.
 * fit_path() is what guarantees a path is never silently clipped; this is just
 * a comfortable default. The delete case is taller because it lists more. */
#define SETUP_PROMPT_W 560
#define SETUP_PROMPT_H 150
#define SETUP_PROMPT_DEL_H 240

/* Height reserved below the log pane for the Start row and the status lines. */
#define SETUP_FOOTER  86

static volatile sig_atomic_t g_setup_stop = 0;

void setup_gui_request_stop(void)
{
    g_setup_stop = 1;
}

/* ---------------------------------------------------------------- log ring */

size_t setup_log_append(char *buf, size_t cap, size_t len, const char *data, size_t n)
{
    if (n >= cap) {
        /* Bigger than the whole ring: keep the newest tail. */
        memcpy(buf, data + (n - cap), cap);
        buf[cap] = '\0';
        return cap;
    }

    if (len + n > cap) {
        size_t need = len + n - cap;
        size_t drop = need;
        /* Evict whole lines: a partial first line renders as garbage. */
        while (drop < len && buf[drop - 1] != '\n')
            drop++;
        memmove(buf, buf + drop, len - drop);
        len -= drop;
    }

    memcpy(buf + len, data, n);
    len += n;
    buf[len] = '\0';
    return len;
}

const char *setup_log_view(const char *buf, size_t len, size_t view)
{
    if (len <= view)
        return buf;
    const char *start = buf + len - view;
    const char *nl = strchr(start, '\n');
    return nl ? nl + 1 : start;
}

static void logf_append(struct setup_gui *g, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void logf_append(struct setup_gui *g, const char *fmt, ...)
{
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if ((size_t)n >= sizeof(line))
        n = (int)sizeof(line) - 1;
    g->log_len = setup_log_append(g->log, SETUP_LOG_CAP, g->log_len, line, (size_t)n);
    g->log_follow = 1;
}

/* ------------------------------------------------------------- config write */

int setup_write_selection(const char *local, const char *example,
                          const char *whisper_path, const char *llama_path)
{
    if (config_bootstrap_local(local, example) < 0)
        return -1;

    const char *keys[] = { "whisper_model_path", "llama_model_path" };
    const char *vals[] = { whisper_path, llama_path };
    return config_write_keys(local, keys, vals, 2);
}

static void write_selection(struct setup_gui *g)
{
    const char *w = g->sel_whisper >= 0 ? g->cat->e[g->sel_whisper].path : "";
    const char *l = g->sel_llama   >= 0 ? g->cat->e[g->sel_llama].path   : "";

    if (setup_write_selection(CONFIG_LOCAL_PATH, CONFIG_EXAMPLE_PATH, w, l) == 0) {
        snprintf(g->status, sizeof(g->status), "wrote %s", CONFIG_LOCAL_PATH);
        logf_append(g, "wrote %s\n  whisper_model_path=%s\n  llama_model_path=%s\n",
                    CONFIG_LOCAL_PATH, w, l[0] ? l : "(none -- cleanup disabled)");
    } else {
        snprintf(g->status, sizeof(g->status),
                 "could not write %s -- see the terminal", CONFIG_LOCAL_PATH);
    }
}

/* ------------------------------------------------------------ model removal */

int setup_path_within(const char *path, const char *root)
{
    if (!path || !root || !*path || !*root)
        return 0;

    size_t rl = strlen(root);
    while (rl > 1 && root[rl - 1] == '/')   /* "/a/b/" and "/a/b" are the same root */
        rl--;

    if (strncmp(path, root, rl) != 0)
        return 0;
    if (path[rl] == '\0')                   /* the root itself */
        return 1;
    /* The boundary test. Without it "/home/u/voice-backup" reads as inside
     * "/home/u/voice", and this function authorises deletion. */
    return path[rl] == '/' || (rl == 1 && root[0] == '/');
}

int setup_plan_removal(const char *path, const char *project_root,
                       struct model_removal *out)
{
    memset(out, 0, sizeof(*out));
    if (!path || !*path) {
        snprintf(out->note, sizeof(out->note), "no path to remove");
        return -1;
    }
    if (snprintf(out->link, sizeof(out->link), "%s", path) >= (int)sizeof(out->link)) {
        snprintf(out->note, sizeof(out->note), "path is too long to handle safely");
        return -1;
    }

    /* lstat, not stat: the question here is what *this* name is, not what it
     * points at. stat() on a symlink would report the target and the link
     * itself would never be recognised as one. */
    struct stat lst;
    if (lstat(path, &lst) != 0) {
        snprintf(out->note, sizeof(out->note), "%s does not exist", path);
        return -1;
    }
    if (S_ISDIR(lst.st_mode)) {
        snprintf(out->note, sizeof(out->note),
                 "%s is a directory -- refusing to remove it", path);
        return -1;
    }

    if (S_ISLNK(lst.st_mode)) {
        out->is_symlink = 1;
        /* PATH_MAX, not CONFIG_PATH_MAX: realpath writes up to PATH_MAX bytes
         * and does not know about our smaller buffer. */
        char resolved[PATH_MAX];
        if (!realpath(path, resolved)) {
            /* Dangling link: nothing to free, but removing the stale link is
             * still the right outcome, so allow it. */
            out->ok = 1;
            out->bytes = 0;
            snprintf(out->note, sizeof(out->note),
                     "dangling symlink -- removes the link only, nothing to free");
            return 0;
        }
        if (strlen(resolved) >= sizeof(out->target)) {
            snprintf(out->note, sizeof(out->note),
                     "symlink target path is too long to handle safely");
            return -1;
        }
        snprintf(out->target, sizeof(out->target), "%s", resolved);
        out->target_inside = setup_path_within(resolved, project_root);
    }

    struct stat st;
    long sz = (stat(path, &st) == 0 && S_ISREG(st.st_mode)) ? (long)st.st_size : 0;

    if (!out->is_symlink) {
        out->bytes = sz;
        snprintf(out->note, sizeof(out->note), "removes the file");
    } else if (out->target_inside) {
        /* Where the space actually is. Files under models/ are commonly
         * symlinks into whisper.cpp/models/ (the README's own `ln -sf` step,
         * which is how large-v3-turbo arrives), so deleting
         * only the link would free nothing while the row flipped to "missing"
         * -- a display that lies about the disk. */
        out->bytes = sz;
        snprintf(out->note, sizeof(out->note), "removes the link and its target");
    } else {
        /* Points outside the project: not ours to delete. */
        out->bytes = 0;
        snprintf(out->note, sizeof(out->note),
                 "target is outside the project -- removes the link only, "
                 "and frees nothing");
    }

    out->ok = 1;
    return 0;
}

int setup_apply_removal(const struct model_removal *r)
{
    if (!r || !r->ok) {
        log_error("models: refusing to remove (%s)", r && r->note[0] ? r->note : "no plan");
        return -1;
    }

    /* Target first, and the link ONLY if the target actually went. Deleting the
     * link while its target survives would strand the bytes: nothing under
     * models/ would name them any more, the row would read "missing", and
     * re-downloading would allocate the space a second time. Keeping the link
     * on failure leaves the model working and visible, so the user can see the
     * error and retry -- the difference between a failed delete and a silent
     * disk leak. */
    if (r->target[0] != '\0' && r->target_inside) {
        if (unlink(r->target) != 0) {
            log_error("models: could not remove %s: %s -- leaving %s in place so the "
                      "file stays reachable", r->target, strerror(errno), r->link);
            return -1;
        }
    }
    if (unlink(r->link) != 0) {
        log_error("models: could not remove %s: %s", r->link, strerror(errno));
        return -1;
    }
    return 0;
}

/* Opens the window on whatever the daemon would actually read, rather than on
 * an arbitrary default that silently disagrees with it. */
static void preselect_from_config(struct setup_gui *g)
{
    g->sel_whisper = -1;
    g->sel_llama   = -1;

    const char *path = config_default_path();
    struct app_config cfg;
    if (config_load(path, &cfg) != 0)
        return;

    for (size_t i = 0; i < g->cat->n; i++) {
        const struct model_entry *m = &g->cat->e[i];
        if (strcmp(m->kind, "whisper") == 0 && strcmp(m->path, cfg.whisper_model_path) == 0)
            g->sel_whisper = (int)i;
        else if (strcmp(m->kind, "llama") == 0 && cfg.llama_model_path[0] != '\0' &&
                 strcmp(m->path, cfg.llama_model_path) == 0)
            g->sel_llama = (int)i;
    }

    logf_append(g, "config: %s\n", path);
    if (g->sel_whisper < 0)
        logf_append(g, "note: whisper_model_path='%s' is not a catalog entry -- "
                       "pick one below to change it\n", cfg.whisper_model_path);
    /* Without this the list would show "(*) none (raw whisper)" while the
     * config actually names a cleanup model -- the display would be lying
     * about what the daemon does today. */
    if (g->sel_llama < 0 && cfg.llama_model_path[0] != '\0')
        logf_append(g, "note: llama_model_path='%s' is not a catalog entry, so no cleanup "
                       "row is marked -- picking one below replaces it\n", cfg.llama_model_path);
}

/* ------------------------------------------------------------------ download */

static void start_download(struct setup_gui *g, int index)
{
    struct model_entry *m = &g->cat->e[index];

    if (download_start(&g->dl, m->url, m->path, m->size) != 0) {
        snprintf(g->status, sizeof(g->status), "could not start curl -- see the terminal");
        return;
    }
    g->dl_active = 1;
    g->dl_index  = index;
    snprintf(g->status, sizeof(g->status), "downloading %s", m->filename);
    /* The exact command first, so a failure can be reproduced by pasting it. */
    logf_append(g, "\n$ %s\n", g->dl.cmdline);
}

static void drain_download(struct setup_gui *g)
{
    char buf[4096];
    for (;;) {
        int got = download_read(&g->dl, buf, sizeof(buf));
        if (got <= 0)
            return;
        g->log_len = setup_log_append(g->log, SETUP_LOG_CAP, g->log_len, buf, (size_t)got);
        g->log_follow = 1;
    }
}

/* Returns 1 if anything the window shows may have changed. */
static int service_download(struct setup_gui *g)
{
    if (!g->dl_active)
        return 0;

    drain_download(g);
    download_update_progress(&g->dl);

    if (download_reap(&g->dl) != 0) {
        /* Whatever curl wrote between the last read and its exit is still in
         * the pipe -- and on a fast failure that is the entire error message,
         * which is the whole reason this pane exists. Reaping is not the end of
         * reading. (Same trap as the D2 CLI loop.) */
        while (g->dl.out_fd >= 0) {
            char buf[4096];
            int got = download_read(&g->dl, buf, sizeof(buf));
            if (got < 0)
                break;
            if (got > 0) {
                g->log_len = setup_log_append(g->log, SETUP_LOG_CAP, g->log_len, buf, (size_t)got);
                g->log_follow = 1;
            }
        }

        int index = g->dl_index;
        g->dl_active = 0;
        catalog_refresh_presence(g->cat, g->models_dir);

        if (g->dl.exit_code == 0) {
            snprintf(g->status, sizeof(g->status), "downloaded %s", g->cat->e[index].filename);
            /* A user who presses Get without ever clicking a name would
             * otherwise still face a disabled Start. Only for whisper: for
             * cleanup, -1 means the deliberate "none" choice, not "unset". */
            if (g->sel_whisper < 0 && strcmp(g->cat->e[index].kind, "whisper") == 0) {
                g->sel_whisper = index;
                write_selection(g);
            }
        } else {
            snprintf(g->status, sizeof(g->status),
                     "curl exited %d -- see the output below", g->dl.exit_code);
        }
    }
    return 1;
}

/* ------------------------------------------------------------ daemon control */

/* Poll cadence. Faster while models load so the status line tracks the
 * transition; slow otherwise, since the only thing that changes it is the user
 * pressing Super+D behind our back. */
#define DAEMON_POLL_MS       1000
#define DAEMON_POLL_FAST_MS   300

static const char *daemon_state_text(enum daemon_state s)
{
    switch (s) {
    case DAEMON_STOPPED: return "not running";
    case DAEMON_LOADING: return "loading models...";
    case DAEMON_READY:   return "ready";
    default:             return "unknown";
    }
}

/* Runs `<script> <arg>` and captures its stdout. Blocking, but only ever used
 * for `status`, which is a pgrep plus a grep -- `start`/`stop` go through
 * daemon_action() instead, because the script's `stop` waits up to 5s for the
 * daemon to exit and that must never freeze the render loop. */
static int run_script_capture(const char *script, const char *arg, char *buf, size_t n)
{
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0)
        return -1;

    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (pid == 0) {
        close(fds[0]);
        if (dup2(fds[1], STDOUT_FILENO) < 0)
            _exit(127);
        if (fds[1] != STDOUT_FILENO)
            close(fds[1]);
        /* stderr is left alone so script errors surface in the terminal. */
        execl(script, script, arg, (char *)NULL);
        _exit(127);
    }

    close(fds[1]);              /* before reading, or EOF never arrives */

    size_t len = 0;
    while (len < n - 1) {
        ssize_t got = read(fds[0], buf + len, n - 1 - len);
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0)
            break;
        len += (size_t)got;
    }
    buf[len] = '\0';
    close(fds[0]);

    int status;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    return 0;
}

/* Returns 1 if the displayed state changed. */
static int poll_daemon(struct setup_gui *g)
{
    if (!g->script_ok)
        return 0;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long ms = (now.tv_sec - g->last_poll.tv_sec) * 1000 +
              (now.tv_nsec - g->last_poll.tv_nsec) / 1000000;
    long want = (g->daemon == DAEMON_LOADING) ? DAEMON_POLL_FAST_MS : DAEMON_POLL_MS;
    if (g->last_poll.tv_sec != 0 && ms < want)
        return 0;
    g->last_poll = now;

    enum daemon_state prev = g->daemon;
    char out[512];
    if (run_script_capture(g->script_path, "status", out, sizeof(out)) != 0)
        g->daemon = DAEMON_UNKNOWN;
    /* Match the fully-quoted forms: "notactive" contains "active", but
     * `"class":"active"` is not a substring of `"class":"notactive"`. */
    else if (strstr(out, "\"class\":\"active\""))
        g->daemon = DAEMON_READY;
    else if (strstr(out, "\"class\":\"loading\""))
        g->daemon = DAEMON_LOADING;
    else if (strstr(out, "\"class\":\"notactive\""))
        g->daemon = DAEMON_STOPPED;
    else
        g->daemon = DAEMON_UNKNOWN;

    return g->daemon != prev;
}

/* Fire-and-forget `<script> start|stop`. Double-forks so the grandchild is
 * reparented to init: `stop` can take seconds, and nothing here may block. */
static void daemon_action(struct setup_gui *g, const char *arg)
{
    if (!g->script_ok)
        return;

    pid_t pid = fork();
    if (pid < 0) {
        snprintf(g->status, sizeof(g->status), "fork failed -- could not %s the daemon", arg);
        return;
    }
    if (pid == 0) {
        pid_t grandchild = fork();
        if (grandchild == 0) {
            execl(g->script_path, g->script_path, arg, (char *)NULL);
            _exit(127);
        }
        _exit(grandchild < 0 ? 1 : 0);
    }

    /* Reaped with WNOHANG on the tick, not waited on here: a blocking wait in
     * a click handler is a frozen window if anything goes wrong. */
    g->pending_action = pid;
    g->last_poll.tv_sec = 0;    /* re-poll status on the next tick */
    snprintf(g->status, sizeof(g->status), "running: waybar-dictation.sh %s", arg);
    logf_append(g, "\n$ %s %s\n", g->script_path, arg);
}

static void reap_pending_action(struct setup_gui *g)
{
    if (!g->pending_action)
        return;
    int status;
    pid_t got = waitpid(g->pending_action, &status, WNOHANG);
    if (got == g->pending_action || (got < 0 && errno != EINTR))
        g->pending_action = 0;
}

/* The launcher script is the one the Super+D keybind and Waybar already use;
 * reimplementing start/stop in C would create a second launcher that could
 * disagree with it about process identity. Located relative to the project
 * directory, and everything daemon-related is disabled (with the reason shown)
 * if it isn't there. */
static void find_script(struct setup_gui *g, const char *project_dir)
{
    int n = snprintf(g->script_path, sizeof(g->script_path),
                     "%s/scripts/waybar-dictation.sh", project_dir);
    if (n < 0 || (size_t)n >= sizeof(g->script_path) || access(g->script_path, X_OK) != 0) {
        g->script_ok = 0;
        log_warn("setup: '%s' not executable -- Start/Stop disabled", g->script_path);
        return;
    }
    g->script_ok = 1;
    log_info("setup: launcher script %s", g->script_path);
}

/* ------------------------------------------------------------------ drawing */

static unsigned long alloc_pixel(struct setup_gui *g, mu_Color c)
{
    unsigned int key = ((unsigned)c.r << 16) | ((unsigned)c.g << 8) | c.b;
    for (int i = 0; i < g->n_colors; i++)
        if (g->color_key[i] == key)
            return g->color_pixel[i];

    XColor xc;
    xc.red   = (unsigned short)(c.r * 257); /* 255*257 == 65535 */
    xc.green = (unsigned short)(c.g * 257);
    xc.blue  = (unsigned short)(c.b * 257);
    xc.flags = DoRed | DoGreen | DoBlue;
    if (!XAllocColor(g->dpy, g->cmap, &xc))
        xc.pixel = BlackPixel(g->dpy, g->screen);

    if (g->n_colors < SETUP_MAX_COLORS) {
        g->color_key[g->n_colors]   = key;
        g->color_pixel[g->n_colors] = xc.pixel;
        g->n_colors++;
    }
    return xc.pixel;
}

/* Renders `path` into `out` so that it is guaranteed to fit within `max_px`,
 * eliding the middle if it does not.
 *
 * mu_text only ever breaks a line at a space (microui.c:714-716, and its inner
 * loop refuses to break at all when the word is the first on the line), so a
 * path -- which contains no spaces -- is never wrapped. It is drawn at full
 * width and silently clipped by the X clip rect, which in the delete dialog
 * truncates the very path the confirmation exists to disclose, and does so
 * without any visual sign that it happened.
 *
 * Width is measured with the real font rather than assumed: the window asks for
 * 9x15 but falls back to 6px 'fixed' when that is not installed, so any
 * hardcoded characters-per-line estimate is wrong on one machine or the other.
 * The tail is grown first because it ends in the filename, which is the part
 * that says what is about to be deleted; the full untruncated path is also
 * written to the log pane when the dialog opens. */
static void fit_path(mu_Context *ctx, const char *path, int max_px, char *out, size_t cap)
{
    if (cap == 0)
        return;
    mu_Font f = ctx->style->font;
    size_t len = strlen(path);

    if (max_px <= 0 || ctx->text_width(f, path, (int)len) <= max_px) {
        snprintf(out, cap, "%s", path);
        return;
    }

    static const char ell[] = "...";
    int ell_w = ctx->text_width(f, ell, 3);

    size_t tail = 0;
    while (tail < len &&
           ctx->text_width(f, path + len - (tail + 1), (int)(tail + 1)) + ell_w <= max_px)
        tail++;
    /* Whatever budget the tail did not use goes to leading context. */
    size_t head = 0;
    while (head + tail < len &&
           ctx->text_width(f, path, (int)(head + 1)) + ell_w +
               ctx->text_width(f, path + len - tail, (int)tail) <= max_px)
        head++;

    snprintf(out, cap, "%.*s%s%.*s", (int)head, path, ell, (int)tail, path + len - tail);
}

/* Appends to a bounded buffer, returning the new length.
 *
 * snprintf returns what it WOULD have written, not what it did, so the obvious
 * `n += snprintf(buf + n, sizeof(buf) - n, ...)` walks n past the buffer the
 * moment anything truncates -- and then `sizeof(buf) - n` underflows (it is
 * size_t) to a huge value while `buf + n` already points past the end, so the
 * NEXT call writes off the end of the frame. The delete dialog concatenates a
 * 96-byte display name and two CONFIG_PATH_MAX paths, which overruns a 640-byte
 * buffer on a long enough --models-dir. Same clamping discipline logf_append
 * uses. */
static size_t msg_append(char *buf, size_t cap, size_t used, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

static size_t msg_append(char *buf, size_t cap, size_t used, const char *fmt, ...)
{
    if (cap == 0 || used >= cap - 1)
        return used;

    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(buf + used, cap - used, fmt, ap);
    va_end(ap);
    if (w < 0)
        return used;

    used += (size_t)w;
    return used < cap ? used : cap - 1;   /* truncated: park at the NUL */
}

/* Works out what removing this entry entails and puts the dialog up. Nothing
 * is deleted here: the plan is computed first precisely so the dialog can name
 * every path before the user commits to anything. */
static void open_delete_prompt(struct setup_gui *g, int index)
{
    struct model_entry *m = &g->cat->e[index];

    if (setup_plan_removal(m->path, g->project_root, &g->prompt_removal) != 0) {
        snprintf(g->status, sizeof(g->status), "cannot remove %s", m->filename);
        logf_append(g, "cannot remove %s: %s\n", m->path, g->prompt_removal.note);
        /* The plan can fail because the file vanished underneath us, so bring
         * the displayed present/missing state back in line with the disk. */
        catalog_refresh_presence(g->cat, g->models_dir);
        return;
    }

    g->prompt_index   = index;
    g->prompt_kind    = PROMPT_DELETE;
    g->prompt_pending = 1;

    /* The dialog elides long paths to fit its width, so record the exact
     * strings elsewhere. Both the pane and the terminal: the pane is where the
     * user is looking, but it wraps on spaces just as the dialog does and a
     * pathological path could clip there too, whereas stderr has no width. */
    logf_append(g, "remove %s?\n  deletes %s\n", m->display, g->prompt_removal.link);
    log_info("setup: remove %s -- deletes %s", m->id, g->prompt_removal.link);
    if (g->prompt_removal.target[0] != '\0') {
        logf_append(g, "  %s %s\n",
                    g->prompt_removal.target_inside ? "deletes" : "keeps  ",
                    g->prompt_removal.target);
        log_info("setup: remove %s -- %s %s", m->id,
                 g->prompt_removal.target_inside ? "deletes" : "keeps",
                 g->prompt_removal.target);
    }
}

static void do_delete(struct setup_gui *g, int index)
{
    struct model_entry *m = &g->cat->e[index];
    const struct model_removal *r = &g->prompt_removal;
    long freed = r->bytes;

    if (setup_apply_removal(r) != 0) {
        snprintf(g->status, sizeof(g->status), "could not remove %s -- see the terminal",
                 m->filename);
        logf_append(g, "failed to remove %s\n", m->path);
    } else {
        snprintf(g->status, sizeof(g->status), "removed %s (%ld MB freed)",
                 m->filename, freed >> 20);
        logf_append(g, "removed %s (%s, %ld MB freed)\n", r->link, r->note, freed >> 20);
        if (r->target[0] != '\0' && r->target_inside)
            logf_append(g, "  also removed %s\n", r->target);
    }

    /* Either way the disk may have changed; re-read it rather than assuming.
     * The selection is deliberately left alone: a deleted model that is still
     * selected now shows "missing", and Start offers to fetch it back. */
    catalog_refresh_presence(g->cat, g->models_dir);
}

/* One catalog row: marker, name (selects), status, Get. Every row is wrapped in
 * a pushed id because mu_button derives its control id from the label string --
 * several buttons labelled "Get" in one container would otherwise collide, and
 * hover/clicks would land on the wrong row. */
static void model_row(struct setup_gui *g, int index)
{
    mu_Context *ctx = g->ctx;
    struct model_entry *m = &g->cat->e[index];
    int is_whisper = strcmp(m->kind, "whisper") == 0;
    int *sel = is_whisper ? &g->sel_whisper : &g->sel_llama;

    mu_push_id(ctx, &index, sizeof(index));
    mu_layout_row(ctx, 4, (int[]){ 30, -160, 60, -1 }, 0);

    /* The marker is its own column rather than part of the button label:
     * mu_button hashes the label, so a label flipping "( )" -> "(*)" would
     * change the control's id between frames. */
    mu_label(ctx, *sel == index ? "(*)" : "( )");

    if (mu_button(ctx, m->display)) {
        *sel = index;
        write_selection(g);
    }

    mu_label(ctx, m->present ? "ready" : "missing");

    /* Column 4 is the per-row action, and which one it is follows the state:
     * [Get] on a missing model, [Remove] on one that is present. Both go inert
     * while a download runs -- for [Remove] that is not cosmetic, since curl
     * renaming its .part onto a path being unlinked concurrently is a race
     * with no good outcome. */
    int opt = MU_OPT_ALIGNCENTER | (g->dl_active ? MU_OPT_NOINTERACT : 0);
    if (m->present) {
        if (mu_button_ex(ctx, "Remove", 0, opt) && !g->dl_active)
            open_delete_prompt(g, index);
    } else {
        if (mu_button_ex(ctx, "Get", 0, opt) && !g->dl_active)
            start_download(g, index);
    }

    mu_pop_id(ctx);
}

static void none_row(struct setup_gui *g)
{
    mu_Context *ctx = g->ctx;
    int key = -1;

    mu_push_id(ctx, &key, sizeof(key));
    mu_layout_row(ctx, 4, (int[]){ 30, -160, 60, -1 }, 0);
    mu_label(ctx, g->sel_llama < 0 ? "(*)" : "( )");
    if (mu_button(ctx, "none (raw whisper)")) {
        g->sel_llama = -1;
        write_selection(g);
    }
    mu_label(ctx, "");
    mu_label(ctx, "");
    mu_pop_id(ctx);
}

/* Catalog index of the first selected-but-not-downloaded model, or -1 if
 * everything the daemon will need is on disk. Whisper is checked first so the
 * prompt names it first.
 *
 * The cleanup model is checked too, which the original Start gate did not do.
 * That was a real hole: config_validate() (src/config.c:148-152) treats a
 * llama_model_path naming a missing file as fatal, so selecting an
 * undownloaded cleanup model and pressing Start produced exactly the
 * config_validate fatal this phase exists to remove. A *blank* llama path is
 * still fine -- that is the "none (raw whisper)" row, and it must never gate
 * Start. */
static int first_missing_required(const struct setup_gui *g)
{
    if (g->sel_whisper >= 0 && !g->cat->e[g->sel_whisper].present)
        return g->sel_whisper;
    if (g->sel_llama >= 0 && !g->cat->e[g->sel_llama].present)
        return g->sel_llama;
    return -1;
}

/* The "that model isn't downloaded yet" prompt.
 *
 * Deliberately NOT mu_begin_popup(): that hardcodes MU_OPT_AUTOSIZE and
 * mu_open_popup() anchors at the mouse, which puts the box at the Start button
 * -- bottom-left, where it would grow straight off the bottom edge. Using
 * mu_begin_window_ex directly gives a fixed size that can be centred, with the
 * rect re-asserted every frame (the same technique the root window needs at
 * the top of render(), and for the same reason).
 *
 * MU_OPT_POPUP is kept so a click outside dismisses it; microui signals that by
 * clearing cnt->open, which is read back BEFORE the rect is re-asserted. */
static void download_prompt(struct setup_gui *g)
{
    mu_Context *ctx = g->ctx;
    if (g->prompt_index < 0)
        return;

    mu_Container *pc = mu_get_container(ctx, SETUP_PROMPT);
    if (!pc || !pc->open) {          /* dismissed by clicking elsewhere */
        g->prompt_index = -1;
        return;
    }

    const struct model_entry *m = &g->cat->e[g->prompt_index];
    int want_h = (g->prompt_kind == PROMPT_DELETE) ? SETUP_PROMPT_DEL_H : SETUP_PROMPT_H;
    int w = g->width  < SETUP_PROMPT_W ? g->width  : SETUP_PROMPT_W;
    int h = g->height < want_h ? g->height : want_h;
    pc->rect = mu_rect((g->width - w) / 2, (g->height - h) / 2, w, h);

    if (!mu_begin_window_ex(ctx, SETUP_PROMPT, pc->rect,
                            MU_OPT_POPUP | MU_OPT_NORESIZE | MU_OPT_NOSCROLL |
                            MU_OPT_NOTITLE | MU_OPT_NOCLOSE))
        return;

    /* Inert while a download runs. For Download that is because there is one
     * struct download, so a second start would overwrite the live one and
     * orphan the running curl child along with its pipe fd; for Delete it is
     * because curl renaming its .part onto a path being unlinked concurrently
     * is a race with no good outcome.
     *
     * Reachable either way, and worth spelling out because a static read of
     * in_hover_root() suggests otherwise: MU_OPT_POPUP does NOT stop a click
     * reaching the control behind it. hover_root is resolved from the PREVIOUS
     * frame's next_hover_root, and moving the pointer to that control -- which
     * a real mouse necessarily does -- already switched hover_root back to the
     * main window before the press lands. So one click both dismisses the
     * dialog and presses the button under it. Verified live: with the dialog
     * open, clicking a catalog row selected that row and wrote the config. */
    int busy = g->dl_active;
    char msg[1024];

    if (g->prompt_kind == PROMPT_DELETE) {
        const struct model_removal *r = &g->prompt_removal;
        size_t n = msg_append(msg, sizeof(msg), 0, "Remove %s?\n\n", m->display);

        /* Elided to the body width so a long path cannot be silently clipped.
         * "deletes  " is 9 characters of prefix that the path must share the
         * row with. */
        int avail = pc->rect.w - 2 * ctx->style->padding
                    - ctx->text_width(ctx->style->font, "deletes  ", 9) - 6;
        char shown[CONFIG_PATH_MAX];

        /* Every path that will be unlinked is named here. Disclosure is what
         * makes the delete safe -- particularly for the symlink case, where
         * what gets deleted is not the path shown in the model list. */
        fit_path(ctx, r->link, avail, shown, sizeof(shown));
        n = msg_append(msg, sizeof(msg), n, "deletes  %s\n", shown);
        if (r->target[0] != '\0') {
            fit_path(ctx, r->target, avail, shown, sizeof(shown));
            n = msg_append(msg, sizeof(msg), n, "%s  %s\n",
                           r->target_inside ? "deletes " : "keeps   ", shown);
        }
        n = msg_append(msg, sizeof(msg), n, "frees    %ld MB\n", r->bytes >> 20);

        /* The window's Start gate would catch a config pointing at a deleted
         * model, but Super+D does not -- it goes straight to the script, to the
         * daemon, to config_validate's fatal. Say so before, not after. */
        if (g->prompt_index == g->sel_whisper || g->prompt_index == g->sel_llama)
            n = msg_append(msg, sizeof(msg), n,
                           "\nThis is the model your config uses: Super+D will fail "
                           "until you pick another or re-download it.");
        (void)n;

        mu_layout_row(ctx, 1, (int[]){ -1 }, -34);
        mu_text(ctx, msg);

        mu_layout_row(ctx, 2, (int[]){ -110, -1 }, 0);
        if (mu_button_ex(ctx, busy ? "Delete (busy)" : "Delete", 0,
                         MU_OPT_ALIGNCENTER | (busy ? MU_OPT_NOINTERACT : 0)) && !busy) {
            do_delete(g, g->prompt_index);
            g->prompt_index = -1;
            pc->open = 0;
        }
    } else {
        snprintf(msg, sizeof(msg),
                 "%s is not downloaded yet.\n\nIt is needed before dictation can "
                 "start. Download it now (%ld MB)?",
                 m->display, m->size > 0 ? (long)(m->size >> 20) : 0L);

        mu_layout_row(ctx, 1, (int[]){ -1 }, -34);
        mu_text(ctx, msg);

        mu_layout_row(ctx, 2, (int[]){ -110, -1 }, 0);
        if (mu_button_ex(ctx, busy ? "Download now (busy)" : "Download now", 0,
                         MU_OPT_ALIGNCENTER | (busy ? MU_OPT_NOINTERACT : 0)) && !busy) {
            /* Reuses the same path the [Get] button takes -- one downloader,
             * one set of states. Deliberately does NOT chain into starting the
             * daemon when it finishes: silently launching after a multi-GB
             * download is surprising, so the user presses Start again. */
            start_download(g, g->prompt_index);
            g->prompt_index = -1;
            pc->open = 0;
        }
    }

    if (mu_button(ctx, "Cancel")) {
        g->prompt_index = -1;
        pc->open = 0;
    }

    mu_end_window(ctx);
}

/* microui has no progress widget; this is the ~8 lines it would be. */
static void progress_bar(struct setup_gui *g)
{
    mu_Context *ctx = g->ctx;
    mu_layout_row(ctx, 1, (int[]){ -1 }, 0);
    mu_Rect r = mu_layout_next(ctx);
    mu_draw_rect(ctx, r, ctx->style->colors[MU_COLOR_BASE]);

    char text[96];
    if (g->dl.dest[0] == '\0') {
        snprintf(text, sizeof(text), "no download yet");
    } else if (g->dl.expected_bytes > 0) {
        double frac = (double)g->dl.got_bytes / (double)g->dl.expected_bytes;
        if (frac < 0.0) frac = 0.0;
        if (frac > 1.0) frac = 1.0;
        mu_Rect fill = r;
        fill.w = (int)(r.w * frac);
        mu_draw_rect(ctx, fill, ctx->style->colors[MU_COLOR_BUTTONFOCUS]);
        snprintf(text, sizeof(text), "%.0f%%   %ld / %ld MB", frac * 100.0,
                 g->dl.got_bytes >> 20, g->dl.expected_bytes >> 20);
    } else {
        /* Unknown total: no bar to draw, just the byte counter. */
        snprintf(text, sizeof(text), "%ld MB (total size unknown)", g->dl.got_bytes >> 20);
    }
    mu_draw_control_text(ctx, text, r, MU_COLOR_TEXT, MU_OPT_ALIGNCENTER);
}

static void render(struct setup_gui *g)
{
    mu_Context *ctx = g->ctx;

    mu_begin(ctx);

    /* mu_begin_window_ex only applies its rect the first time a container is
     * seen (`if (cnt->rect.w == 0)`), so the window would keep its original
     * 560x640 no matter what size the WM gave it. That is not a corner case:
     * a tiling WM resizes on map, which left the content in a corner of the
     * frame with the footer cut off. Re-assert the rect every frame. */
    mu_Container *root = mu_get_container(ctx, SETUP_TITLE);
    if (root)
        root->rect = mu_rect(0, 0, g->width, g->height);

    /* NOTITLE: the window manager draws the real titlebar. NOSCROLL: only the
     * output pane scrolls, so there is nothing for a second scrollbar to do. */
    if (mu_begin_window_ex(ctx, SETUP_TITLE, mu_rect(0, 0, g->width, g->height),
                           MU_OPT_NOTITLE | MU_OPT_NORESIZE | MU_OPT_NOCLOSE |
                           MU_OPT_NOSCROLL)) {
        mu_layout_row(ctx, 1, (int[]){ -1 }, 0);
        mu_label(ctx, "Whisper model (speech recognition)");
        for (size_t i = 0; i < g->cat->n; i++)
            if (strcmp(g->cat->e[i].kind, "whisper") == 0)
                model_row(g, (int)i);

        mu_layout_row(ctx, 1, (int[]){ -1 }, 0);
        mu_label(ctx, "Cleanup model (optional)");
        none_row(g);
        for (size_t i = 0; i < g->cat->n; i++)
            if (strcmp(g->cat->e[i].kind, "llama") == 0)
                model_row(g, (int)i);

        progress_bar(g);

        /* Negative height: extend to SETUP_FOOTER px above the bottom, leaving
         * exactly enough for the Start button and the two status lines. */
        mu_layout_row(ctx, 1, (int[]){ -1 }, -SETUP_FOOTER);
        mu_begin_panel(ctx, "output");
        mu_Container *pane = mu_get_current_container(ctx);
        mu_layout_row(ctx, 1, (int[]){ -1 }, -1);
        mu_text(ctx, g->log_len ? setup_log_view(g->log, g->log_len, SETUP_LOG_VIEW)
                                : "(no output yet)");
        if (g->log_follow) {
            /* Overshoot deliberately: microui clamps scroll to the content, and
             * content_size is only known one frame later. */
            pane->scroll.y = pane->content_size.y;
            g->log_follow = 0;
        }
        mu_end_panel(ctx);

        int missing   = first_missing_required(g);
        int can_start = g->sel_whisper >= 0 && missing < 0;
        int is_up = (g->daemon == DAEMON_READY || g->daemon == DAEMON_LOADING);

        /* Clickable when a model is merely *missing*, so the prompt can offer to
         * fetch it -- but still inert with nothing selected, no launcher script,
         * or a download already running. The original gate's purpose (never hand
         * the user config_validate's fatal from behind a button) is preserved:
         * a click with something missing opens the prompt, it does not start
         * the daemon. */
        int start_ok = g->sel_whisper >= 0 && g->script_ok && !g->dl_active;

        mu_layout_row(ctx, 2, (int[]){ 150, -1 }, 0);
        if (is_up) {
            /* Stop is never gated: whatever state the config is in, the user
             * must always be able to shut the daemon down. */
            if (mu_button(ctx, "Stop dictation"))
                daemon_action(g, "stop");
        } else {
            /* Gated, because local.conf is seeded from example.conf, which
             * names models a fresh clone doesn't have. An ungated Start would
             * hand the user config_validate's fatal error -- exactly the
             * failure this phase exists to remove -- from behind a button. */
            int opt = MU_OPT_ALIGNCENTER | (start_ok ? 0 : MU_OPT_NOINTERACT);
            if (mu_button_ex(ctx, "Start dictation", 0, opt) && start_ok) {
                if (missing >= 0) {
                    /* mu_open_popup, not just setting prompt_index: it also
                     * makes the popup the hover root, without which this very
                     * click would immediately close it again (MU_OPT_POPUP
                     * closes on any press landing outside the container). */
                    g->prompt_index   = missing;
                    g->prompt_kind    = PROMPT_DOWNLOAD;
                    g->prompt_pending = 1;
                } else {
                    daemon_action(g, "start");
                }
            }
        }

        char why[192];
        if (!g->script_ok)
            snprintf(why, sizeof(why), "scripts/waybar-dictation.sh not found -- "
                                       "Start/Stop unavailable");
        /* Without this the button is simply dead during a download, with the
         * line below it explaining something else entirely. */
        else if (!is_up && g->dl_active)
            snprintf(why, sizeof(why), "daemon: %s -- downloading; Start waits until it finishes",
                     daemon_state_text(g->daemon));
        else if (!is_up && g->sel_whisper < 0)
            snprintf(why, sizeof(why), "daemon: %s -- pick a whisper model first",
                     daemon_state_text(g->daemon));
        else if (!is_up && !can_start)
            snprintf(why, sizeof(why), "daemon: %s -- %s still needs downloading",
                     daemon_state_text(g->daemon), g->cat->e[missing].filename);
        else
            snprintf(why, sizeof(why), "daemon: %s", daemon_state_text(g->daemon));
        mu_label(ctx, why);

        mu_layout_row(ctx, 1, (int[]){ -1 }, 0);
        mu_label(ctx, g->status);

        /* Must be inside the window, at the same id-stack depth as the
         * mu_open_popup call above -- mu_begin_window_ex pushes the window's id
         * (microui.c:1088) and mu_get_id seeds its hash from the top of that
         * stack (microui.c:232), so the *same* name outside this block hashes
         * to a different container. Opening one container while drawing another
         * makes MU_OPT_POPUP's "close when a press lands elsewhere" test
         * compare against the wrong container, and the prompt closes on the
         * very frame it opens. It is not clipped by the parent: root containers
         * push unclipped_rect in begin_root_container. */
        /* The single place the dialog is opened, deliberately right next to
         * where it is drawn so the two can never drift to different id-stack
         * depths again. */
        if (g->prompt_pending) {
            mu_open_popup(ctx, SETUP_PROMPT);
            g->prompt_pending = 0;
        }
        download_prompt(g);

        mu_end_window(ctx);
    }

    mu_end(ctx);

    /* ---- rasterize into the backbuffer (same as gui_xlib.c) ---- */
    XSetClipMask(g->dpy, g->gc, None);
    XSetForeground(g->dpy, g->gc, g->bg_pixel);
    XFillRectangle(g->dpy, g->backbuf, g->gc, 0, 0, g->width, g->height);

    mu_Command *cmd = NULL;
    while (mu_next_command(ctx, &cmd)) {
        switch (cmd->type) {
        case MU_COMMAND_CLIP: {
            /* Clamped before narrowing -- microui's "no clip" sentinel is
             * 0x1000000 wide and truncates to 0 in XRectangle's unsigned short,
             * a 0x0 clip that silently discards every later draw. This bit here
             * first (D3: the curl output and footer vanished); the logic now
             * lives in src/xclip.c so the panel and this window cannot drift
             * apart on it. */
            XRectangle xr;
            xclip_clamp(cmd->clip.rect.x, cmd->clip.rect.y,
                        cmd->clip.rect.w, cmd->clip.rect.h,
                        g->width, g->height, &xr);
            XSetClipRectangles(g->dpy, g->gc, 0, 0, &xr, 1, Unsorted);
            break;
        }
        case MU_COMMAND_RECT:
            XSetForeground(g->dpy, g->gc, alloc_pixel(g, cmd->rect.color));
            XFillRectangle(g->dpy, g->backbuf, g->gc,
                           cmd->rect.rect.x, cmd->rect.rect.y,
                           cmd->rect.rect.w, cmd->rect.rect.h);
            break;
        case MU_COMMAND_TEXT: {
            XSetForeground(g->dpy, g->gc, alloc_pixel(g, cmd->text.color));
            /* microui text pos is the top-left; Xlib draws from the baseline. */
            int baseline = cmd->text.pos.y + g->font.fs->ascent;
            XDrawString(g->dpy, g->backbuf, g->gc,
                        cmd->text.pos.x, baseline,
                        cmd->text.str, (int)strlen(cmd->text.str));
            break;
        }
        case MU_COMMAND_ICON:
            /* Stays a no-op, as in gui_xlib.c. The layout above deliberately
             * uses no widget that emits an icon command (no mu_checkbox, no
             * mu_header, no treenodes, and the window is NOTITLE|NOCLOSE), so
             * implementing icons would be dead code. If a later revision adds
             * mu_header sections, icons must be implemented at the same time or
             * those headers render blank. */
            break;
        }
    }

    XSetClipMask(g->dpy, g->gc, None);
    XCopyArea(g->dpy, g->backbuf, g->win, g->gc, 0, 0, g->width, g->height, 0, 0);
    XFlush(g->dpy);
}

/* ------------------------------------------------------------------- events */

static int handle_x_event(struct setup_gui *g, XEvent *ev)
{
    switch (ev->type) {
    case Expose:
        return 1;
    case ClientMessage:
        /* Without this the WM's close button just drops the connection, and
         * Xlib aborts the process with an XIO fatal error. */
        if ((Atom)ev->xclient.data.l[0] == g->wm_delete)
            g->running = 0;
        return 0;
    case KeyPress: {
        KeySym ks = XLookupKeysym(&ev->xkey, 0);
        if (ks == XK_Escape)
            g->running = 0;
        return 0;   /* the only keyboard path: there are no text fields */
    }
    case ConfigureNotify:
        if (ev->xconfigure.width != g->width || ev->xconfigure.height != g->height) {
            g->width  = ev->xconfigure.width;
            g->height = ev->xconfigure.height;
            XFreePixmap(g->dpy, g->backbuf);
            g->backbuf = XCreatePixmap(g->dpy, g->win, g->width, g->height,
                                       DefaultDepth(g->dpy, g->screen));
            return 1;
        }
        return 0;
    case MotionNotify:
        mu_input_mousemove(g->ctx, ev->xmotion.x, ev->xmotion.y);
        return 1;
    case ButtonPress:
    case ButtonRelease: {
        int x = ev->xbutton.x, y = ev->xbutton.y;
        int btn = 0;
        switch (ev->xbutton.button) {
        case Button1: btn = MU_MOUSE_LEFT;   break;
        case Button2: btn = MU_MOUSE_MIDDLE; break;
        case Button3: btn = MU_MOUSE_RIGHT;  break;
        case Button4: if (ev->type == ButtonPress) mu_input_scroll(g->ctx, 0, -30); return 1;
        case Button5: if (ev->type == ButtonPress) mu_input_scroll(g->ctx, 0,  30); return 1;
        default: return 0;
        }
        if (ev->type == ButtonPress)
            mu_input_mousedown(g->ctx, x, y, btn);
        else
            mu_input_mouseup(g->ctx, x, y, btn);
        return 1;
    }
    default:
        return 0;
    }
}

/* --------------------------------------------------------------- lifecycle */

int setup_gui_init(struct setup_gui *g, Display *dpy, struct model_catalog *cat,
                   const char *models_dir, const char *font_xlfd,
                   const char *project_dir)
{
    memset(g, 0, sizeof(*g));
    g->dpy        = dpy;
    g->cat        = cat;
    g->models_dir = models_dir;
    g->screen     = DefaultScreen(dpy);
    g->width      = SETUP_WIDTH;
    g->height     = SETUP_HEIGHT;
    g->cmap       = DefaultColormap(dpy, g->screen);
    g->bg_pixel   = BlackPixel(dpy, g->screen);
    g->dl.out_fd  = -1;

    if (font_open(&g->font, dpy, (font_xlfd && font_xlfd[0]) ? font_xlfd : SETUP_FONT) != 0)
        return -1;

    Window root = RootWindow(dpy, g->screen);

    XSetWindowAttributes attrs;
    /* No CWOverrideRedirect: it stays False, so unlike the status panel this
     * window is managed by the WM -- titlebar, focus, taskbar entry. */
    attrs.background_pixel = g->bg_pixel;
    attrs.event_mask = ExposureMask | StructureNotifyMask | KeyPressMask |
                       ButtonPressMask | ButtonReleaseMask | PointerMotionMask;

    g->win = XCreateWindow(dpy, root, 0, 0, g->width, g->height, 0,
                           CopyFromParent, InputOutput, CopyFromParent,
                           CWBackPixel | CWEventMask, &attrs);
    if (!g->win) {
        log_error("setup: XCreateWindow failed");
        font_close(&g->font);
        return -1;
    }

    XStoreName(dpy, g->win, SETUP_TITLE);

    /* Ask for a floor on the size. A tiling WM is free to ignore this (and
     * Hyprland does), which is why the layout degrades rather than breaks. */
    XSizeHints hints;
    memset(&hints, 0, sizeof(hints));
    hints.flags = PMinSize;
    hints.min_width = SETUP_MIN_W;
    hints.min_height = SETUP_MIN_H;
    XSetWMNormalHints(dpy, g->win, &hints);

    /* res_class must equal StartupWMClass in the .desktop file (D5) or the
     * window shows up as a separate unnamed dock entry. */
    XClassHint hint = { (char *)"dictation-setup", (char *)"Dictation" };
    XSetClassHint(dpy, g->win, &hint);

    g->wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, g->win, &g->wm_delete, 1);

    g->backbuf = XCreatePixmap(dpy, g->win, g->width, g->height,
                               DefaultDepth(dpy, g->screen));
    g->gc = XCreateGC(dpy, g->win, 0, NULL);
    XSetFont(dpy, g->gc, g->font.fs->fid);

    g->ctx = malloc(sizeof(mu_Context));
    if (!g->ctx) {
        log_error("setup: out of memory allocating mu_Context");
        setup_gui_destroy(g);
        return -1;
    }
    mu_init(g->ctx);
    g->ctx->text_width  = font_text_width;
    g->ctx->text_height = font_text_height;
    g->ctx->style->font = (mu_Font)g->font.fs;
    /* Default control height is 20px, which crowds a 15px font. */
    g->ctx->style->size.y = 16;

    /* Explicit: the memset above leaves this 0, which is a *valid* catalog
     * index, so the prompt would be up the moment the window opened. */
    g->prompt_index = -1;
    g->prompt_kind  = PROMPT_DOWNLOAD;

    /* The containment anchor for deletes, resolved once. Comparing a resolved
     * symlink target against an unresolved anchor gives false negatives when
     * the project is reached through a symlinked path -- and a false negative
     * here silently downgrades "delete the target too" to "delete the link and
     * free nothing". Left empty if it cannot be resolved, which makes every
     * target read as outside the project: the safe direction to fail. */
    {
        char resolved[PATH_MAX];
        const char *anchor = (project_dir && *project_dir) ? project_dir : ".";
        if (realpath(anchor, resolved) && strlen(resolved) < sizeof(g->project_root))
            snprintf(g->project_root, sizeof(g->project_root), "%s", resolved);
        else
            log_warn("setup: cannot resolve project directory '%s' -- symlinked models "
                     "will have their link removed but their target kept", anchor);
    }

    preselect_from_config(g);
    find_script(g, project_dir);
    snprintf(g->status, sizeof(g->status), "pick a model; missing ones download with [Get]");

    XMapWindow(dpy, g->win);
    XFlush(dpy);

    log_info("setup: window created (%dx%d, WM-managed)", g->width, g->height);
    return 0;
}

int setup_gui_run(struct setup_gui *g)
{
    g_setup_stop = 0;
    g->running = 1;

    while (g->running && !g_setup_stop) {
        int need_render = 0;

        /* Drain what Xlib already buffered -- poll() never wakes for events
         * that have already been read off the socket into Xlib's queue. */
        while (XPending(g->dpy)) {
            XEvent ev;
            XNextEvent(g->dpy, &ev);
            need_render |= handle_x_event(g, &ev);
        }

        need_render |= service_download(g);
        reap_pending_action(g);
        need_render |= poll_daemon(g);

        if (need_render)
            render(g);
        if (!g->running || g_setup_stop)
            break;

        struct pollfd fds[2];
        fds[0].fd = ConnectionNumber(g->dpy);
        fds[0].events = POLLIN;
        fds[0].revents = 0;
        int nfds = 1;
        if (g->dl_active && g->dl.out_fd >= 0) {
            fds[1].fd = g->dl.out_fd;
            fds[1].events = POLLIN;
            fds[1].revents = 0;
            nfds = 2;
        }

        /* The change from the status panel's purely event-driven loop: the
         * progress bar is computed by re-stat()ing the .part file, so it has to
         * tick even when curl emits nothing between meter updates -- and the
         * daemon's state can change behind our back (Super+D), so that needs a
         * tick too. Fully event-driven only when neither applies. */
        int timeout = -1;
        if (g->dl_active)
            timeout = 250;
        else if (g->script_ok)
            timeout = (g->daemon == DAEMON_LOADING) ? DAEMON_POLL_FAST_MS : DAEMON_POLL_MS;
        int rc = poll(fds, nfds, timeout);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            log_error("setup: poll() failed: %s", strerror(errno));
            break;
        }
    }

    log_info("setup: window closing");
    return 0;
}

void setup_gui_destroy(struct setup_gui *g)
{
    /* Every exit path -- Esc, the WM close button, SIGINT -- lands here, so
     * this is the single place that guarantees no orphan curl is left writing
     * into a .part file. */
    if (g->dl_active) {
        log_info("setup: cancelling the download in progress");
        download_cancel(&g->dl);
        g->dl_active = 0;
    }

    if (g->ctx) {
        free(g->ctx);
        g->ctx = NULL;
    }
    if (g->gc) {
        XFreeGC(g->dpy, g->gc);
        g->gc = NULL;
    }
    if (g->backbuf) {
        XFreePixmap(g->dpy, g->backbuf);
        g->backbuf = 0;
    }
    if (g->win) {
        XDestroyWindow(g->dpy, g->win);
        g->win = 0;
    }
    font_close(&g->font);
}
