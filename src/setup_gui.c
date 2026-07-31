#include "setup_gui.h"
#include "config.h"
#include "config_write.h"
#include "log.h"

#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

    if (m->present) {
        mu_label(ctx, "");
    } else {
        /* One download at a time: the others go inert while one runs. */
        int opt = MU_OPT_ALIGNCENTER | (g->dl_active ? MU_OPT_NOINTERACT : 0);
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

        int can_start = g->sel_whisper >= 0 && g->cat->e[g->sel_whisper].present;

        mu_layout_row(ctx, 2, (int[]){ 150, -1 }, 0);
        int opt = MU_OPT_ALIGNCENTER | (can_start ? 0 : MU_OPT_NOINTERACT);
        if (mu_button_ex(ctx, "Start dictation", 0, opt) && can_start)
            snprintf(g->status, sizeof(g->status),
                     "not wired yet (D4) -- run: scripts/waybar-dictation.sh start");
        mu_label(ctx, can_start ? "ready to start"
                                : "select a whisper model and Get it first");

        mu_layout_row(ctx, 1, (int[]){ -1 }, 0);
        mu_label(ctx, g->status);
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
            /* Clamp to the window before narrowing to XRectangle's 16-bit
             * fields. This is not defensive tidying: microui's "no clip"
             * sentinel is `{0, 0, 0x1000000, 0x1000000}` (microui.c:50), and
             * 0x1000000 truncated to an unsigned short is 0 -- a 0x0 clip that
             * silently discards every later draw. mu_draw_text emits that
             * sentinel to restore the clip after any partially-clipped string,
             * so with a scrolling log pane it fired on nearly every frame and
             * erased the whole footer. */
            mu_Rect r = cmd->clip.rect;
            int cx = r.x < 0 ? 0 : (r.x > g->width  ? g->width  : r.x);
            int cy = r.y < 0 ? 0 : (r.y > g->height ? g->height : r.y);
            int cw = r.w < 0 ? 0 : (r.w > g->width  - cx ? g->width  - cx : r.w);
            int ch = r.h < 0 ? 0 : (r.h > g->height - cy ? g->height - cy : r.h);
            XRectangle xr = { (short)cx, (short)cy,
                              (unsigned short)cw, (unsigned short)ch };
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
                   const char *models_dir, const char *font_xlfd)
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

    preselect_from_config(g);
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

        /* The one change from the status panel's purely event-driven loop: the
         * progress bar is computed by re-stat()ing the .part file, so it has to
         * tick even when curl emits nothing between meter updates. */
        int rc = poll(fds, nfds, g->dl_active ? 250 : -1);
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
