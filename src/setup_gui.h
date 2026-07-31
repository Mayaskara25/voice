#ifndef DICTATION_SETUP_GUI_H
#define DICTATION_SETUP_GUI_H

#include "downloader.h"
#include "font_xlib.h"
#include "model_catalog.h"

#include <X11/Xlib.h>
#include <microui.h>
#include <stddef.h>

/* The model-picker window (Phase D3).
 *
 * A sibling of gui_xlib.c, not an edit of it: the status panel's behaviour is
 * load-bearing and must not regress, so the rasterizer, colour cache,
 * backbuffer blit and poll skeleton are reused *by copy*. Every property below
 * is deliberately the opposite of the panel's:
 *
 *   - override_redirect stays False. This window is WM-managed, focusable and
 *     appears in the taskbar -- the exact inverse of the panel's defining
 *     property (the panel must never take focus, or injected keystrokes would
 *     land in it instead of the user's editor).
 *   - WM_DELETE_WINDOW is handled. Without it, clicking the window manager's
 *     close button drops the X connection and Xlib aborts the process.
 *   - KeyPressMask is on, used only for Esc-to-close. There are no editable
 *     text fields anywhere: that is what keeps this window minimal (no
 *     XLookupString / mu_input_text / mu_textbox plumbing at all).
 *
 * Single-threaded, like the panel: the download is a child process whose pipe
 * joins the X connection in one poll(). */

#define SETUP_MAX_COLORS 64

/* Ring capacity vs. how much of it is rendered. mu_text emits one text command
 * per wrapped line and microui hard-aborts if the 256 KB command list
 * overflows, so only the tail is drawn -- the rest stays available for
 * scrolling back through what curl printed. */
#define SETUP_LOG_CAP  (64 * 1024)
#define SETUP_LOG_VIEW (8 * 1024)

struct setup_gui {
    Display *dpy;
    int      screen;
    Window   win;
    Pixmap   backbuf;
    GC       gc;
    Colormap cmap;
    int      width, height;
    unsigned long bg_pixel;
    Atom     wm_delete;

    struct font_ctx font;
    mu_Context     *ctx;            /* heap-allocated (256KB command list) */

    unsigned int  color_key[SETUP_MAX_COLORS];
    unsigned long color_pixel[SETUP_MAX_COLORS];
    int           n_colors;

    struct model_catalog *cat;      /* borrowed */
    const char           *models_dir;
    int sel_whisper;                /* catalog index, -1 = none chosen yet */
    int sel_llama;                  /* catalog index, -1 = "none (raw whisper)" */

    struct download dl;
    int dl_active;
    int dl_index;                   /* catalog index being fetched */

    char   log[SETUP_LOG_CAP + 1];  /* +1: always NUL-terminated for mu_text */
    size_t log_len;
    int    log_follow;              /* new output arrived -> stick to the bottom */

    char status[192];
    int  running;
};

/* Creates the window, backbuffer, GC, font and mu_Context, and pre-selects
 * whatever the current config already points at. `dpy` and `cat` are borrowed.
 * `font_xlfd` may be NULL/empty for the default. Returns 0, or -1 logged. */
int  setup_gui_init(struct setup_gui *g, Display *dpy, struct model_catalog *cat,
                    const char *models_dir, const char *font_xlfd);

/* Runs the poll() loop over the X connection plus, while a download is live,
 * curl's pipe. Returns 0 on a normal exit (Esc, the WM close button, or
 * setup_gui_request_stop). */
int  setup_gui_run(struct setup_gui *g);

/* Async-signal-safe stop request (mirrors gui_request_stop). */
void setup_gui_request_stop(void);

/* Cancels any running download, then frees the X resources. */
void setup_gui_destroy(struct setup_gui *g);

/* --- pieces split out so they can be tested without an X display --- */

/* Appends `n` bytes to a NUL-terminated ring of capacity `cap`, evicting whole
 * oldest lines when it would overflow (a half-line at the top renders as
 * garbage). Returns the new length. A single append larger than the ring keeps
 * its tail. */
size_t setup_log_append(char *buf, size_t cap, size_t len, const char *data, size_t n);

/* Returns the start of the last `view` bytes of `buf`, advanced to the next
 * line boundary so the pane never opens with a truncated first line. */
const char *setup_log_view(const char *buf, size_t len, size_t view);

/* Seeds `local` from `example` if needed, then writes the two model paths into
 * it comment-preservingly. An empty llama_path disables cleanup at runtime,
 * which is what the "none (raw whisper)" entry selects. Returns 0 or -1. */
int setup_write_selection(const char *local, const char *example,
                          const char *whisper_path, const char *llama_path);

#endif
