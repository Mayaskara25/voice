#include "font_xlib.h"
#include "log.h"

#include <string.h>

int font_open(struct font_ctx *f, Display *dpy, const char *xlfd)
{
    f->dpy = dpy;
    f->fs = NULL;

    /* Track what actually loaded, not what was asked for: reporting the
     * requested name after a fallback makes a layout that renders at the
     * wrong size look inexplicable. */
    const char *loaded = "fixed";

    if (xlfd && xlfd[0] != '\0') {
        f->fs = XLoadQueryFont(dpy, xlfd);
        if (f->fs)
            loaded = xlfd;
    }

    if (!f->fs) {
        if (xlfd && xlfd[0] != '\0')
            log_warn("font: could not load '%s', falling back to 'fixed'", xlfd);
        f->fs = XLoadQueryFont(dpy, "fixed");
    }

    if (!f->fs) {
        log_error("font: could not load any X core font (not even 'fixed')");
        return -1;
    }

    log_info("font: loaded '%s' (ascent=%d descent=%d)",
             loaded, f->fs->ascent, f->fs->descent);
    return 0;
}

void font_close(struct font_ctx *f)
{
    if (f->fs) {
        XFreeFont(f->dpy, f->fs);
        f->fs = NULL;
    }
}

int font_text_width(mu_Font font, const char *str, int len)
{
    XFontStruct *fs = (XFontStruct *)font;
    if (!fs || !str)
        return 0;
    if (len < 0)
        len = (int)strlen(str);
    return XTextWidth(fs, str, len);
}

int font_text_height(mu_Font font)
{
    XFontStruct *fs = (XFontStruct *)font;
    if (!fs)
        return 0;
    return fs->ascent + fs->descent;
}
