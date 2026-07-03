#ifndef DICTATION_INJECT_XTEST_H
#define DICTATION_INJECT_XTEST_H

#include <X11/Xlib.h>

/* Opens the X11 display (shared by the injector and, in Phase C, the GUI
 * thread). Returns NULL on failure. */
Display *inject_open_display(void);

void inject_close_display(Display *dpy);

/* Types `text` (English/ASCII, plus '\n'/'\t') into whichever window
 * currently holds X input focus, via XTest key synthesis. Returns 0 on
 * success, -1 if the display is unusable. Unmappable characters are skipped
 * with a warning rather than aborting the whole string. */
int inject_type_text(Display *dpy, const char *text);

#endif
