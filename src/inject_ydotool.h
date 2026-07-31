#ifndef DICTATION_INJECT_YDOTOOL_H
#define DICTATION_INJECT_YDOTOOL_H

/* Verifies the `ydotool` CLI is on PATH and ydotoold's socket has a live
 * listener. Returns 0 if ready; -1 with a specific, actionable log_error
 * identifying which half failed otherwise. Cheap and side-effect-free; safe
 * to call more than once. */
int inject_ydotool_check(void);

/* Types `text` by invoking the `ydotool` CLI (fork+execvp, never a shell --
 * dictated text may contain quotes/backticks/$). ASCII-only, matching
 * inject_xtest.c: bytes outside 0x20-0x7E (excluding '\n'/'\t') are skipped
 * with a log_warn. Returns 0 on success (including partial-skip cases); -1
 * only if `text` is NULL. A failed chunk (spawn/nonzero exit) is logged and
 * skipped, never aborting the rest of the string. */
int inject_ydotool_type_text(const char *text);

#endif
