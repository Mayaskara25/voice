#include "gui_xlib.h"
#include "xclip.h"
#include "app_state.h"
#include "inject.h"
#include "log.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define GUI_WIDTH   300
#define GUI_HEIGHT  100
#define GUI_MARGIN  24

/* Stop flag + wakeup fd are file-scope so gui_request_stop() can be called from
 * a signal handler with no arguments (mirrors hotkey_request_stop()). Writing
 * one byte to the wake fd unblocks the poll(); the flag ends the loop. */
static volatile sig_atomic_t g_gui_stop = 0;
static int g_gui_wake_fd = -1;

void gui_request_stop(void)
{
    g_gui_stop = 1;
    if (g_gui_wake_fd >= 0) {
        const unsigned char byte = 1;
        ssize_t n = write(g_gui_wake_fd, &byte, 1);
        (void)n; /* best-effort wake; loop also checks g_gui_stop */
    }
}

static unsigned long alloc_pixel(struct gui *g, mu_Color c)
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

    if (g->n_colors < GUI_MAX_COLORS) {
        g->color_key[g->n_colors]   = key;
        g->color_pixel[g->n_colors] = xc.pixel;
        g->n_colors++;
    }
    return xc.pixel;
}

int gui_init(struct gui *g, Display *dpy, struct ipc_handoff *ipc, const char *font_xlfd,
             enum inject_backend backend)
{
    memset(g, 0, sizeof(*g));
    g->dpy    = dpy;
    g->ipc    = ipc;
    g->inject_backend = backend;
    g->screen = DefaultScreen(dpy);
    g->width  = GUI_WIDTH;
    g->height = GUI_HEIGHT;
    g->cmap   = DefaultColormap(dpy, g->screen);
    g->bg_pixel = BlackPixel(dpy, g->screen);

    if (font_open(&g->font, dpy, font_xlfd) != 0)
        return -1;

    Window root = RootWindow(dpy, g->screen);
    int x = DisplayWidth(dpy, g->screen) - g->width - GUI_MARGIN;
    int y = GUI_MARGIN;

    XSetWindowAttributes attrs;
    /* override_redirect=True: the window manager ignores this window entirely,
     * so it cannot be given input focus. This is the load-bearing mechanism for
     * focus preservation -- do not change it. */
    attrs.override_redirect = True;
    attrs.background_pixel  = g->bg_pixel;
    attrs.event_mask = ExposureMask | StructureNotifyMask |
                       ButtonPressMask | ButtonReleaseMask | PointerMotionMask;

    g->win = XCreateWindow(dpy, root, x, y, g->width, g->height, 0,
                           CopyFromParent, InputOutput, CopyFromParent,
                           CWOverrideRedirect | CWBackPixel | CWEventMask, &attrs);
    if (!g->win) {
        log_error("gui: XCreateWindow failed");
        font_close(&g->font);
        return -1;
    }

    g->backbuf = XCreatePixmap(dpy, g->win, g->width, g->height,
                               DefaultDepth(dpy, g->screen));
    g->gc = XCreateGC(dpy, g->win, 0, NULL);
    XSetFont(dpy, g->gc, g->font.fs->fid);

    g->ctx = malloc(sizeof(mu_Context));
    if (!g->ctx) {
        log_error("gui: out of memory allocating mu_Context");
        gui_destroy(g);
        return -1;
    }
    mu_init(g->ctx);
    g->ctx->text_width  = font_text_width;
    g->ctx->text_height = font_text_height;
    g->ctx->style->font = (mu_Font)g->font.fs;

    /* XMapWindow (NOT XMapRaised): map without requesting a stacking raise that
     * some setups treat as a focus grab. */
    XMapWindow(dpy, g->win);
    XFlush(dpy);

    log_info("gui: status panel created (%dx%d, override-redirect)", g->width, g->height);
    return 0;
}

static void render(struct gui *g)
{
    mu_Context *ctx = g->ctx;

    enum app_state st = ipc_get_state(g->ipc);
    char transcript[512];
    ipc_get_transcript(g->ipc, transcript, sizeof(transcript));

    /* ---- build one microui frame ---- */
    mu_begin(ctx);
    if (mu_begin_window_ex(ctx, "dictation", mu_rect(0, 0, g->width, g->height),
                           MU_OPT_NOTITLE | MU_OPT_NORESIZE | MU_OPT_NOCLOSE |
                           MU_OPT_NOSCROLL)) {
        char status[80];
        snprintf(status, sizeof(status), "state: %s", app_state_name(st));
        mu_layout_row(ctx, 1, (int[]){ -1 }, 0);
        mu_label(ctx, status);
        mu_layout_row(ctx, 1, (int[]){ -1 }, -1);
        mu_text(ctx, transcript[0] ? transcript : "(waiting for input)");
        mu_end_window(ctx);
    }
    mu_end(ctx);

    /* ---- rasterize commands into the backbuffer ---- */
    XSetClipMask(g->dpy, g->gc, None);
    XSetForeground(g->dpy, g->gc, g->bg_pixel);
    XFillRectangle(g->dpy, g->backbuf, g->gc, 0, 0, g->width, g->height);

    mu_Command *cmd = NULL;
    while (mu_next_command(ctx, &cmd)) {
        switch (cmd->type) {
        case MU_COMMAND_CLIP: {
            /* Clamped before narrowing: microui's "no clip" sentinel is
             * 0x1000000 wide, which truncates to 0 in XRectangle's unsigned
             * short and would discard every later draw. See src/xclip.c. */
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
            /* the status panel uses no icons */
            break;
        }
    }

    /* ---- blit to the window ---- */
    XSetClipMask(g->dpy, g->gc, None);
    XCopyArea(g->dpy, g->backbuf, g->win, g->gc, 0, 0, g->width, g->height, 0, 0);
    XFlush(g->dpy);
}

/* Translates an X event into microui input. Returns 1 if a redraw is warranted. */
static int handle_x_event(struct gui *g, XEvent *ev)
{
    switch (ev->type) {
    case Expose:
        return 1;
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

int gui_run(struct gui *g)
{
    g_gui_stop = 0;
    g_gui_wake_fd = g->ipc->pipe_fds[1]; /* write end wakes our poll() */

    struct pollfd fds[2];
    fds[0].fd = ConnectionNumber(g->dpy);
    fds[1].fd = ipc_wakeup_fd(g->ipc);
    fds[0].events = fds[1].events = POLLIN;

    while (!g_gui_stop) {
        int need_render = 0;

        /* Drain any events Xlib already buffered -- poll() won't wake for
         * bytes already read off the socket into Xlib's queue. */
        while (XPending(g->dpy)) {
            XEvent ev;
            XNextEvent(g->dpy, &ev);
            need_render |= handle_x_event(g, &ev);
        }
        if (need_render)
            render(g);

        fds[0].revents = fds[1].revents = 0;
        int rc = poll(fds, 2, -1);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            log_error("gui: poll() failed: %s", strerror(errno));
            break;
        }

        if (fds[1].revents & POLLIN) {
            ipc_drain(g->ipc);
            /* Paint the just-published state FIRST, before consuming the text --
             * otherwise the INJECTING label would be overwritten by IDLE below
             * and never actually shown. */
            render(g);
            char *text = ipc_take_inject_text(g->ipc);
            if (text) {
                /* Injection happens here, on the GUI thread that owns Display;
                 * it blocks, so the INJECTING label stays up for its duration. */
                inject_dispatch_type(g->inject_backend, g->dpy, text);
                free(text);
                /* Return to IDLE only if the worker hasn't already begun a new
                 * utterance (re-pressed PTT) while we were typing. */
                if (ipc_get_state(g->ipc) == APP_STATE_INJECTING)
                    ipc_set_state(g->ipc, APP_STATE_IDLE);
                render(g); /* reflect IDLE (or leave the worker's new state) */
            }
        }
        /* X activity is drained at the top of the next iteration. */
    }

    g_gui_wake_fd = -1;
    log_info("gui: loop exiting");
    return 0;
}

void gui_destroy(struct gui *g)
{
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
