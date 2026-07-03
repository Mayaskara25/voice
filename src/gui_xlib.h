#ifndef DICTATION_GUI_XLIB_H
#define DICTATION_GUI_XLIB_H

#include "font_xlib.h"
#include "ipc_handoff.h"

#include <X11/Xlib.h>
#include <microui.h>

#define GUI_MAX_COLORS 64

/* A borderless, override-redirect status panel rendered with plain Xlib.
 *
 * Focus-preservation is the whole point (PLAN.md decision #9 / correctness
 * rules): the window is created override_redirect=True and NEVER calls any
 * focus-requesting API (XSetInputFocus, focus-raising XMapRaised, focus WM
 * hints). Injected keystrokes must land in whatever app the user was looking
 * at, not here.
 *
 * All fields are touched only by the GUI thread; mu_Context is likewise
 * GUI-thread-only (microui is not thread-safe). */
struct gui {
    Display  *dpy;               /* shared X connection (also used for injection here) */
    int       screen;
    Window    win;
    Pixmap    backbuf;           /* off-screen double buffer to avoid flicker */
    GC        gc;
    Colormap  cmap;
    int       width, height;
    unsigned long bg_pixel;

    struct font_ctx     font;
    mu_Context         *ctx;     /* heap-allocated (256KB command list) */
    struct ipc_handoff *ipc;

    /* tiny mu_Color -> X pixel cache (microui uses a small fixed palette) */
    unsigned int   color_key[GUI_MAX_COLORS];
    unsigned long  color_pixel[GUI_MAX_COLORS];
    int            n_colors;
};

/* Creates the window, backbuffer, GC, font, and mu_Context. Returns 0 on
 * success, -1 on error (message logged). `font_xlfd` may be NULL/empty to use
 * the default. `dpy` and `ipc` are borrowed, not owned. */
int  gui_init(struct gui *g, Display *dpy, struct ipc_handoff *ipc, const char *font_xlfd);

/* Runs the poll() loop over {X connection fd, ipc self-pipe fd} until
 * gui_request_stop() is called. Performs pending text injection on this
 * thread. Returns 0 on normal exit. */
int  gui_run(struct gui *g);

/* Signals the GUI loop to stop and wakes its poll(). Async-signal-safe; may be
 * called from a signal handler. */
void gui_request_stop(void);

void gui_destroy(struct gui *g);

#endif
