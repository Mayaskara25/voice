#ifndef DICTATION_FONT_XLIB_H
#define DICTATION_FONT_XLIB_H

#include <X11/Xlib.h>
#include <microui.h>

/* An Xlib core font wrapped to satisfy microui's text_width/text_height
 * callbacks. The XFontStruct* is handed to microui *as* its opaque mu_Font
 * (stored in ctx->style->font), so the callbacks recover it by cast -- no
 * globals, and it works even if several fonts were ever in play. */
struct font_ctx {
    Display     *dpy;
    XFontStruct *fs;
};

/* Loads the named X core font (XLFD or alias, e.g. "fixed", "9x15"); falls
 * back to "fixed" if the requested font is unavailable. Returns 0 on success,
 * -1 if even "fixed" cannot be loaded. */
int  font_open(struct font_ctx *f, Display *dpy, const char *xlfd);
void font_close(struct font_ctx *f);

/* microui callbacks. `font` is the XFontStruct* passed as mu_Font. */
int  font_text_width(mu_Font font, const char *str, int len);
int  font_text_height(mu_Font font);

#endif
