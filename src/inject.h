#ifndef DICTATION_INJECT_H
#define DICTATION_INJECT_H

#include <X11/Xlib.h>
#include <stdbool.h>

/* Dispatch facade over the available keystroke-injection backends, so the
 * pipeline call sites (main.c, gui_xlib.c) don't need to branch on which one
 * is configured. */
enum inject_backend {
    INJECT_BACKEND_XTEST,   /* X11 XTest key synthesis; needs an open Display */
    INJECT_BACKEND_YDOTOOL, /* uinput via the ydotool CLI; works under any compositor */
};

/* Maps a config string ("xtest"/"ydotool") to the enum. Any unrecognized
 * value (including config_validate()'s already-warned-about bad input)
 * defaults to INJECT_BACKEND_XTEST, so this is always safe to call. */
enum inject_backend inject_backend_parse(const char *name);

/* True if this backend needs an X11 Display connection to inject keystrokes.
 * Lets run_headless() decide whether to open one at all; run_gui() always
 * opens a Display regardless, since GUI rendering needs it either way. */
bool inject_backend_needs_display(enum inject_backend backend);

/* One-time startup check for backends with external runtime dependencies.
 * xtest is a no-op (inject_open_display() already validates the X
 * connection). ydotool verifies its CLI and daemon are actually reachable.
 * Returns 0 if ready, -1 with a logged, actionable error otherwise. Call
 * once at startup so failures surface immediately, not as a cryptic
 * per-keystroke exec failure. */
int inject_backend_check(enum inject_backend backend);

/* Types `text` via the given backend. For INJECT_BACKEND_XTEST, `dpy` must be
 * a valid open Display (from inject_open_display()); for INJECT_BACKEND_YDOTOOL,
 * `dpy` is ignored and may be NULL. Returns 0 on success; unmappable/unresolvable
 * characters are skipped with a per-character warning, never aborting the
 * whole string. */
int inject_dispatch_type(enum inject_backend backend, Display *dpy, const char *text);

#endif
