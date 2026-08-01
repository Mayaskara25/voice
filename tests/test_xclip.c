/* test_xclip: the clip-rectangle clamp shared by both microui rasterizers.
 *
 * Pure arithmetic, no Display opened, so `make test` stays headless-safe. The
 * case that matters is the first one: microui's "no clipping" sentinel is
 * 0x1000000 wide, and a direct cast into XRectangle's unsigned short turns that
 * into 0 -- inverting "clip nothing" into "clip everything" and discarding every
 * subsequent draw. That was a real, hard-to-find rendering bug in both windows,
 * so it is pinned here rather than left to a visual check. */
#include "xclip.h"

#include <stdio.h>

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
} while (0)

/* microui.c:50 */
#define MU_UNCLIPPED 0x1000000

int main(void)
{
    XRectangle r;

    /* The regression assertion. */
    xclip_clamp(0, 0, MU_UNCLIPPED, MU_UNCLIPPED, 300, 100, &r);
    CHECK(r.width == 300 && r.height == 100,
          "the unclipped sentinel clamps to the whole surface, not 0");
    CHECK(r.width != 0 && r.height != 0,
          "the sentinel must never narrow to a 0x0 clip (it would discard all later draws)");
    CHECK(r.x == 0 && r.y == 0, "sentinel origin is the surface origin");

    /* An ordinary in-bounds rect is untouched. */
    xclip_clamp(10, 20, 100, 40, 300, 100, &r);
    CHECK(r.x == 10 && r.y == 20 && r.width == 100 && r.height == 40,
          "an in-bounds rect passes through unchanged");

    /* Exactly the surface. */
    xclip_clamp(0, 0, 300, 100, 300, 100, &r);
    CHECK(r.width == 300 && r.height == 100, "a full-surface rect is unchanged");

    /* Negative origin: microui can emit these while scrolling. */
    xclip_clamp(-50, -10, 100, 40, 300, 100, &r);
    CHECK(r.x == 0 && r.y == 0, "a negative origin clamps to 0");

    /* Oversized extent is trimmed to what is left of the surface. */
    xclip_clamp(250, 80, 999, 999, 300, 100, &r);
    CHECK(r.x == 250 && r.y == 80 && r.width == 50 && r.height == 20,
          "an oversized rect is trimmed to the remaining surface");

    /* Entirely off the surface: zero extent, never a wrapped value. */
    xclip_clamp(500, 500, 100, 100, 300, 100, &r);
    CHECK(r.width == 0 && r.height == 0, "a fully off-surface rect yields an empty clip");

    /* Negative extent must not wrap when narrowed to unsigned. */
    xclip_clamp(0, 0, -5, -5, 300, 100, &r);
    CHECK(r.width == 0 && r.height == 0, "a negative extent yields 0, not a wrapped 65531");

    /* A degenerate surface must not produce nonsense. */
    xclip_clamp(0, 0, MU_UNCLIPPED, MU_UNCLIPPED, 0, 0, &r);
    CHECK(r.width == 0 && r.height == 0, "a zero-sized surface yields an empty clip");

    if (failures) { fprintf(stderr, "test_xclip: %d failure(s)\n", failures); return 1; }
    printf("test_xclip: OK\n");
    return 0;
}
