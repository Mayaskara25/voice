#include "xclip.h"

/* Why this is a function and not an inline cast at each call site.
 *
 * microui signals "no clipping" with a sentinel rectangle rather than a flag:
 *
 *     static mu_Rect unclipped_rect = { 0, 0, 0x1000000, 0x1000000 };
 *
 * (microui.c:50). mu_draw_text emits it to *restore* clipping after drawing a
 * string that was only partially clipped (microui.c:503) -- so it appears in
 * the command stream during ordinary use, not as some edge case.
 *
 * XRectangle's width and height are `unsigned short`. 0x1000000 is 16777216,
 * which is exactly 256 * 65536, so a direct cast truncates it to *precisely
 * zero*. The meaning inverts: "clip nothing" becomes a 0x0 clip that discards
 * every draw command after it. In the setup window this silently erased the
 * curl output and the entire footer, and it survived a forced redraw -- the
 * only way to find it was dumping the command stream.
 *
 * Clamping to the surface before narrowing keeps the sentinel meaning "the
 * whole surface", which is what it is for. Both rasterizers call this so the
 * fix cannot drift between them; tests/test_xclip.c pins the sentinel case. */
void xclip_clamp(int x, int y, int w, int h, int surf_w, int surf_h, XRectangle *out)
{
    if (surf_w < 0) surf_w = 0;
    if (surf_h < 0) surf_h = 0;

    int cx = x < 0 ? 0 : (x > surf_w ? surf_w : x);
    int cy = y < 0 ? 0 : (y > surf_h ? surf_h : y);
    int cw = w < 0 ? 0 : (w > surf_w - cx ? surf_w - cx : w);
    int ch = h < 0 ? 0 : (h > surf_h - cy ? surf_h - cy : h);

    out->x      = (short)cx;
    out->y      = (short)cy;
    out->width  = (unsigned short)cw;
    out->height = (unsigned short)ch;
}
