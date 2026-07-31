#include "inject.h"
#include "inject_xtest.h"
#include "inject_ydotool.h"

#include <string.h>

enum inject_backend inject_backend_parse(const char *name)
{
    if (name && strcmp(name, "ydotool") == 0)
        return INJECT_BACKEND_YDOTOOL;
    return INJECT_BACKEND_XTEST;
}

bool inject_backend_needs_display(enum inject_backend backend)
{
    return backend == INJECT_BACKEND_XTEST;
}

int inject_backend_check(enum inject_backend backend)
{
    if (backend == INJECT_BACKEND_YDOTOOL)
        return inject_ydotool_check();
    return 0;
}

int inject_dispatch_type(enum inject_backend backend, Display *dpy, const char *text)
{
    if (backend == INJECT_BACKEND_YDOTOOL)
        return inject_ydotool_type_text(text);
    return inject_type_text(dpy, text);
}
