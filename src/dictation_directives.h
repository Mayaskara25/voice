#ifndef DICTATION_DIRECTIVES_H
#define DICTATION_DIRECTIVES_H

#include <stdbool.h>

enum dictation_cleanup_override {
    DICTATION_CLEANUP_DEFAULT = 0,
    DICTATION_CLEANUP_SKIP,
    DICTATION_CLEANUP_STYLE,
};

struct dictation_directive_result {
    char *text;                                      /* malloc'd; caller owns */
    enum dictation_cleanup_override cleanup_override;
    char cleanup_style[32];                         /* valid when override == STYLE */
};

/* Applies spoken per-utterance directives at the start of `raw`.
 *
 * Supported examples:
 *   keep all lowercase i am hungry  -> i am hungry, with cleanup skipped
 *   spell word c o d e x            -> codex, with cleanup skipped
 *   literal codex --help            -> codex --help, with cleanup skipped
 *   command echo hello              -> echo hello, cleaned with commands style
 *
 * If no directive is present, returns a copy of raw with DEFAULT cleanup.
 * Returns 0 on success, -1 on allocation failure or empty input.
 */
int dictation_directives_apply(const char *raw, struct dictation_directive_result *out);

void dictation_directives_free(struct dictation_directive_result *res);

#endif
