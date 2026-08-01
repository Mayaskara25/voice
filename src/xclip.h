#ifndef DICTATION_XCLIP_H
#define DICTATION_XCLIP_H

#include <X11/Xlib.h>

/* Clamps a clip rectangle to a surf_w x surf_h surface and narrows it into an
 * XRectangle. Shared by both microui rasterizers (gui_xlib.c's status panel and
 * setup_gui.c's setup window) so the narrowing exists in exactly one place --
 * see xclip.c for why getting it wrong is not a cosmetic matter.
 *
 * Plain ints in rather than mu_Rect, so this file needs nothing from microui. */
void xclip_clamp(int x, int y, int w, int h, int surf_w, int surf_h, XRectangle *out);

#endif
