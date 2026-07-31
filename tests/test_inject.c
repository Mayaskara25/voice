/* test_inject: verifies the backend-selection facade (inject.h) without
 * needing a live X Display -- inject_backend_check() for INJECT_BACKEND_YDOTOOL
 * is exercised against a YDOTOOL_SOCKET forced to a path nothing listens on,
 * so the result is deterministic regardless of whether this machine happens
 * to have a real ydotoold running (it may, on a dev box actually using it). */
#include "inject.h"

#include <stdio.h>
#include <stdlib.h>

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
} while (0)

int main(void)
{
    CHECK(inject_backend_parse("xtest") == INJECT_BACKEND_XTEST, "parses 'xtest'");
    CHECK(inject_backend_parse("ydotool") == INJECT_BACKEND_YDOTOOL, "parses 'ydotool'");
    CHECK(inject_backend_parse("bogus") == INJECT_BACKEND_XTEST, "unknown value defaults to xtest");
    CHECK(inject_backend_parse(NULL) == INJECT_BACKEND_XTEST, "NULL defaults to xtest");

    CHECK(inject_backend_needs_display(INJECT_BACKEND_XTEST) == true, "xtest needs a Display");
    CHECK(inject_backend_needs_display(INJECT_BACKEND_YDOTOOL) == false, "ydotool needs no Display");

    CHECK(inject_backend_check(INJECT_BACKEND_XTEST) == 0, "xtest check is a no-op");

    /* Force a socket path nothing listens on, so this fails fast regardless
     * of ambient host state -- must not hang or crash. */
    setenv("YDOTOOL_SOCKET", "/nonexistent/test-ydotool.sock", 1);
    CHECK(inject_backend_check(INJECT_BACKEND_YDOTOOL) == -1,
          "ydotool check fails cleanly against an unreachable socket");
    unsetenv("YDOTOOL_SOCKET");

    if (failures) { fprintf(stderr, "test_inject: %d failure(s)\n", failures); return 1; }
    printf("test_inject: OK\n");
    return 0;
}
