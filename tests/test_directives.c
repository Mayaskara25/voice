/* test_directives: verifies spoken per-utterance formatting directives before
 * optional LLM cleanup/injection. */
#include "dictation_directives.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
} while (0)

static void check_apply(const char *raw,
                        const char *want_text,
                        enum dictation_cleanup_override want_override,
                        const char *want_style)
{
    struct dictation_directive_result res;
    int rc = dictation_directives_apply(raw, &res);

    CHECK(rc == 0, "dictation_directives_apply succeeds");
    if (rc == 0) {
        CHECK(strcmp(res.text, want_text) == 0, raw);
        CHECK(res.cleanup_override == want_override, raw);
        CHECK(strcmp(res.cleanup_style, want_style ? want_style : "") == 0, raw);
        dictation_directives_free(&res);
    }
}

int main(void)
{
    check_apply("keep all lowercase I am Hungry", "i am hungry",
                DICTATION_CLEANUP_SKIP, NULL);
    check_apply("<keep all lowercase> I am Hungry", "i am hungry",
                DICTATION_CLEANUP_SKIP, NULL);
    check_apply("spell word C O D E X", "codex",
                DICTATION_CLEANUP_SKIP, NULL);
    check_apply("spell C-O-D-E-X", "codex",
                DICTATION_CLEANUP_SKIP, NULL);
    check_apply("literal codex --help", "codex --help",
                DICTATION_CLEANUP_SKIP, NULL);
    check_apply("command echo hello", "echo hello",
                DICTATION_CLEANUP_STYLE, "commands");
    check_apply("code mode const name equals codex", "const name equals codex",
                DICTATION_CLEANUP_STYLE, "code");
    check_apply("ordinary dictation text", "ordinary dictation text",
                DICTATION_CLEANUP_DEFAULT, NULL);

    if (failures) { fprintf(stderr, "test_directives: %d failure(s)\n", failures); return 1; }
    printf("test_directives: OK\n");
    return 0;
}
