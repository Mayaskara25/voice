#include "inject_xtest.h"
#include "log.h"

#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>

#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#define TYPE_DELAY_US 2000 /* small delay between characters for reliability */

Display *inject_open_display(void)
{
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        log_error("inject: cannot open X display (is DISPLAY set / is X11 running?)");
        return NULL;
    }

    int event_base, error_base, major, minor;
    if (!XTestQueryExtension(dpy, &event_base, &error_base, &major, &minor)) {
        log_error("inject: XTest extension not available on this X server");
        XCloseDisplay(dpy);
        return NULL;
    }

    log_info("inject: X display opened, XTest %d.%d", major, minor);
    return dpy;
}

void inject_close_display(Display *dpy)
{
    if (dpy)
        XCloseDisplay(dpy);
}

/* Maps an ASCII character to its KeySym. For printable ASCII (0x20-0x7E) the
 * KeySym value equals the character code itself (Latin-1 keysyms mirror
 * ISO-8859-1 codepoints in that range) -- no lookup table needed. */
static KeySym ascii_to_keysym(char c)
{
    if (c == '\n')
        return XK_Return;
    if (c == '\t')
        return XK_Tab;
    if ((unsigned char)c >= 0x20 && (unsigned char)c <= 0x7E)
        return (KeySym)(unsigned char)c;
    return NoSymbol;
}

/* Finds an otherwise-unused keycode to temporarily bind `keysym` to, for the
 * (English/ASCII-only, so rare) case where the current keymap has no keycode
 * for it at all. This is the standard xdotool-style fallback: X only lets us
 * synthesize events for keysyms that some keycode is currently bound to. */
static KeyCode remap_scratch_keycode(Display *dpy, KeySym keysym)
{
    int min_kc, max_kc;
    XDisplayKeycodes(dpy, &min_kc, &max_kc);

    int keysyms_per_kc = 0;
    KeySym *map = XGetKeyboardMapping(dpy, min_kc, max_kc - min_kc + 1, &keysyms_per_kc);
    if (!map)
        return 0;

    KeyCode scratch = 0;
    for (int kc = max_kc; kc >= min_kc; kc--) {
        bool unused = true;
        for (int lvl = 0; lvl < keysyms_per_kc; lvl++) {
            KeySym s = map[(kc - min_kc) * keysyms_per_kc + lvl];
            if (s != NoSymbol) {
                unused = false;
                break;
            }
        }
        if (unused) {
            scratch = (KeyCode)kc;
            break;
        }
    }
    XFree(map);

    if (scratch == 0) {
        log_warn("inject: no unused keycode available to remap for keysym 0x%lx", keysym);
        return 0;
    }

    KeySym pair[2] = { keysym, keysym }; /* same on both levels: no shift needed */
    XChangeKeyboardMapping(dpy, scratch, 2, pair, 1);
    XSync(dpy, False);
    return scratch;
}

/* Resolves keysym -> (keycode, need_shift). Returns keycode, or 0 if it could
 * not be resolved even via the scratch-keycode fallback. */
static KeyCode resolve_key(Display *dpy, KeySym keysym, bool *need_shift)
{
    *need_shift = false;

    KeyCode kc = XKeysymToKeycode(dpy, keysym);
    if (kc != 0) {
        int keysyms_per_kc = 0;
        KeySym *map = XGetKeyboardMapping(dpy, kc, 1, &keysyms_per_kc);
        if (map) {
            if (keysyms_per_kc > 1 && map[0] != keysym && map[1] == keysym)
                *need_shift = true;
            XFree(map);
        }
        return kc;
    }

    return remap_scratch_keycode(dpy, keysym);
}

static void send_key(Display *dpy, KeyCode kc, bool need_shift)
{
    KeyCode shift_kc = XKeysymToKeycode(dpy, XK_Shift_L);

    if (need_shift)
        XTestFakeKeyEvent(dpy, shift_kc, True, CurrentTime);

    XTestFakeKeyEvent(dpy, kc, True, CurrentTime);
    XTestFakeKeyEvent(dpy, kc, False, CurrentTime);

    if (need_shift)
        XTestFakeKeyEvent(dpy, shift_kc, False, CurrentTime);

    XFlush(dpy);
}

int inject_type_text(Display *dpy, const char *text)
{
    if (!dpy) {
        log_error("inject: no display");
        return -1;
    }

    for (const char *p = text; *p; p++) {
        KeySym keysym = ascii_to_keysym(*p);
        if (keysym == NoSymbol) {
            log_warn("inject: skipping unsupported character 0x%02x", (unsigned char)*p);
            continue;
        }

        bool need_shift = false;
        KeyCode kc = resolve_key(dpy, keysym, &need_shift);
        if (kc == 0) {
            log_warn("inject: could not resolve keycode for character '%c', skipping", *p);
            continue;
        }

        send_key(dpy, kc, need_shift);
        usleep(TYPE_DELAY_US);
    }

    return 0;
}
