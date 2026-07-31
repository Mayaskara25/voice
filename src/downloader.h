#ifndef DICTATION_DOWNLOADER_H
#define DICTATION_DOWNLOADER_H

#include "config.h"   /* CONFIG_PATH_MAX */

#include <stddef.h>
#include <sys/types.h>

/* Model downloads (Phase D), as a child `curl` process rather than libcurl.
 *
 * Why a child process: failures show curl's *actual* output instead of a
 * paraphrase, and LDLIBS stays free of -lcurl, preserving the "no network code
 * in the binary" property the top of PLAN.md states. The exact command is
 * available as `cmdline` so it can be echoed into the log pane and pasted into
 * a terminal to reproduce a failure verbatim.
 *
 * No threads: the caller polls `out_fd` alongside its other fds and ticks
 * download_update_progress()/download_reap(). Everything here is non-blocking
 * except download_cancel(), which is bounded (see below). */

/* Longest command line we build: the two paths plus the fixed flags. */
#define DOWNLOAD_CMDLINE_MAX (2 * CONFIG_PATH_MAX + 64)

struct download {
    pid_t pid;
    int   out_fd;                       /* curl's stdout+stderr, merged, O_NONBLOCK; -1 once EOF */
    char  dest[CONFIG_PATH_MAX];        /* final path, only created on success */
    char  part[CONFIG_PATH_MAX];        /* dest + ".part", what curl actually writes */
    long  expected_bytes;               /* 0 = unknown (catalog size is advisory) */
    long  got_bytes;                    /* stat(part).st_size, refreshed on the tick */
    int   running;                      /* 1 between download_start and a successful reap */
    int   exit_code;                    /* valid once running == 0; 128+signo if killed */
    int   at_line_start;                /* CR-translation state, carried across reads */
    char  cmdline[DOWNLOAD_CMDLINE_MAX];
};

/* Returns 0 if the `curl` binary is on PATH, -1 otherwise with a message
 * logged. Call once at startup rather than per-download: the same fail-fast
 * posture inject_ydotool_check() takes for ydotoold. */
int download_check_curl(void);

/* Forks `curl -L --fail -o <dest>.part <url>`, with stdout+stderr merged onto
 * one non-blocking pipe. Creates dest's parent directory if needed (a fresh
 * clone has no models/ -- git doesn't track empty directories).
 *
 * `expected` is the catalog's advisory size, used only for the progress
 * fraction; 0 means "unknown" and the caller should show bytes-so-far instead.
 *
 * --fail is not optional: without it an HTTP 404 writes the error page into the
 * output file, leaving a ~500-byte "model" that fails incomprehensibly deep
 * inside whisper.cpp. With it curl exits 22 and writes nothing.
 *
 * Returns 0 on success (*d is fully initialized), -1 on failure with a message
 * logged (*d is left not-running and needs no cleanup). */
int download_start(struct download *d, const char *url, const char *dest, long expected);

/* Drains up to `n` bytes of whatever curl has written, never blocking.
 * Returns  >0  bytes placed in buf (already CR-translated, see below),
 *           0  nothing available right now,
 *          -1  end of output: the child closed its pipe. out_fd is closed and
 *              set to -1, so stop polling it and reap.
 * Bytes are NOT NUL-terminated. */
int download_read(struct download *d, char *buf, size_t n);

/* Refreshes got_bytes from stat(part). The progress bar is computed from the
 * file on disk, not from parsing curl's meter, which is why the caller's poll
 * needs a timeout while a download runs: the bar must advance on a tick even
 * when curl emits nothing. */
void download_update_progress(struct download *d);

/* Non-blocking waitpid. Returns 0 while the child is still running, 1 once it
 * has exited (exit_code set, and the download finalized: rename(part, dest) on
 * success, unlink(part) otherwise, so a partial or failed fetch never occupies
 * the final filename), -1 on an unexpected waitpid error. Idempotent -- calling
 * it again after it returned 1 is a no-op returning 1. */
int download_reap(struct download *d);

/* Stops a running download and removes its .part. Bounded: SIGTERM, then poll
 * for up to ~2s, then SIGKILL and a blocking wait -- so this can never hang a
 * GUI teardown or a test. Safe to call on a finished or never-started
 * download. */
void download_cancel(struct download *d);

/* Rewrites curl's progress meter, which overwrites one line using '\r', into
 * something appendable to a scrolling log: '\r' becomes '\n' and runs of
 * newlines collapse to one. Without this the whole meter lands in the buffer as
 * a single enormous unreadable line.
 *
 * Translates in place and returns the new (never larger) length.
 * *at_line_start carries the collapse state across reads; initialize it to 1 so
 * leading blank lines are suppressed too. Pure apart from that state, so it can
 * be unit-tested without a child process. */
size_t download_translate_cr(char *buf, size_t n, int *at_line_start);

#endif
