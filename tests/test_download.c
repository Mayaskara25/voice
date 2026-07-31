/* test_download: exercises the curl child-process downloader -- CR translation,
 * the fork/exec/pipe/reap cycle, .part -> dest promotion on success, .part
 * removal on failure, and bounded cancellation.
 *
 * No network: the transfers use file:// URLs, which curl supports natively, so
 * `make test` stays offline and deterministic. That covers everything except
 * --fail itself, which is an HTTP-only option (see PLAN.md, Phase D2). */
#include "downloader.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
} while (0)

static void append(char *log, size_t logsz, const char *buf, size_t got)
{
    size_t used = strlen(log);
    size_t room = logsz - used - 1;
    size_t take = got < room ? got : room;
    memcpy(log + used, buf, take);
    log[used + take] = '\0';
}

/* Accumulates whatever is readable right now into `log` (NUL-terminated). */
static void sink(struct download *d, char *log, size_t logsz)
{
    char buf[4096];
    for (;;) {
        int got = download_read(d, buf, sizeof(buf));
        if (got <= 0)
            return;                 /* 0 = nothing right now, -1 = EOF */
        append(log, logsz, buf, (size_t)got);
    }
}

/* Reads the pipe out after the child has exited. Its only writer is gone, so
 * read() returns the remaining bytes and then EOF and can no longer say EAGAIN;
 * the iteration cap is paranoia, not flow control. */
static void drain_to_eof(struct download *d, char *log, size_t logsz)
{
    char buf[4096];
    for (int i = 0; i < 10000 && d->out_fd >= 0; i++) {
        int got = download_read(d, buf, sizeof(buf));
        if (got < 0)
            return;
        if (got > 0)
            append(log, logsz, buf, (size_t)got);
    }
}

/* Drives a download to completion the way the CLI (and D3's window) does:
 * poll, read on any event, tick progress, reap, then drain the tail. Bounded so
 * a stuck child can never hang the suite. Returns the exit code, or -999 on
 * timeout. Everything curl wrote lands in `log`. */
static int drive(struct download *d, char *log, size_t logsz, int timeout_ms)
{
    log[0] = '\0';
    int waited = 0;
    for (;;) {
        if (d->out_fd >= 0) {
            struct pollfd p = { .fd = d->out_fd, .events = POLLIN, .revents = 0 };
            int r = poll(&p, 1, 50);
            /* Read on ANY event: curl's exit shows up as POLLHUP with no
             * POLLIN, and a POLLIN-only loop would spin instead of seeing it. */
            if (r > 0 && p.revents != 0)
                sink(d, log, logsz);
        } else {
            struct timespec ts = { .tv_nsec = 10 * 1000 * 1000 };
            nanosleep(&ts, NULL);
        }
        download_update_progress(d);
        if (download_reap(d) != 0) {
            /* Whatever curl wrote between the last read and its exit is still
             * in the pipe. On a fast transfer that is the entire error message,
             * so reaping is not the end of reading. */
            drain_to_eof(d, log, logsz);
            return d->exit_code;
        }
        waited += 50;
        if (waited > timeout_ms) {
            download_cancel(d);
            return -999;
        }
    }
}

static int exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static void test_cr_translation(void)
{
    int state = 1;
    char a[] = "\r\r\r";
    CHECK(download_translate_cr(a, 3, &state) == 0, "a leading run of CRs collapses to nothing");

    state = 1;
    char b[] = "abc\r\ndef\r";
    size_t n = download_translate_cr(b, sizeof(b) - 1, &state);
    CHECK(n == 8 && memcmp(b, "abc\ndef\n", 8) == 0, "CR becomes NL and \\r\\n collapses to one NL");

    state = 1;
    char c[] = "  5 100k    5 5000\r 10 100k   10 10000\r";
    n = download_translate_cr(c, sizeof(c) - 1, &state);
    CHECK(memchr(c, '\r', n) == NULL, "no CR survives translation");
    CHECK(n > 0 && c[n - 1] == '\n', "a meter update ends the line");
    int lines = 0;
    for (size_t i = 0; i < n; i++)
        if (c[i] == '\n')
            lines++;
    CHECK(lines == 2, "two meter updates become two lines, not one giant one");

    /* State carried across reads: a run split over two calls still collapses. */
    state = 1;
    char d1[] = "x\r";
    char d2[] = "\ny\n";
    n = download_translate_cr(d1, 2, &state);
    CHECK(n == 2, "first chunk keeps its newline");
    n = download_translate_cr(d2, 3, &state);
    CHECK(n == 2 && memcmp(d2, "y\n", 2) == 0,
          "a newline run split across two reads still collapses");
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    test_cr_translation();

    if (download_check_curl() != 0) {
        printf("test_download: SKIP (no curl on PATH -- CR translation still checked)\n");
        return failures ? 1 : 0;
    }

    char dir[] = "/tmp/dictation_dl_XXXXXX";
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }

    char src[256], url[512], dest[256], part[512], fifo[256];
    snprintf(src, sizeof(src), "%s/source.bin", dir);
    snprintf(url, sizeof(url), "file://%s", src);
    /* Into a subdirectory that does not exist yet: a fresh clone has no models/. */
    snprintf(dest, sizeof(dest), "%s/models/model.bin", dir);
    snprintf(part, sizeof(part), "%s.part", dest);
    snprintf(fifo, sizeof(fifo), "%s/stalled", dir);

    const long size = 100000;
    FILE *f = fopen(src, "w");
    if (!f) { perror("fopen"); return 1; }
    for (long i = 0; i < size; i++)
        fputc('x', f);
    fclose(f);

    /* --- success: .part is promoted to dest --- */
    struct download d;
    CHECK(download_start(&d, url, dest, size) == 0, "download_start forks curl");
    CHECK(strstr(d.cmdline, "--fail") != NULL, "the echoed command carries --fail");
    CHECK(strstr(d.cmdline, ".part") != NULL, "curl writes to the .part path, not dest");
    CHECK(exists(dest) == 0, "dest does not exist while the download runs");

    static char log[65536];
    int rc = drive(&d, log, sizeof(log), 20000);
    CHECK(rc == 0, "curl exits 0 for a readable file:// url");
    CHECK(log[0] != '\0', "curl's progress meter reached the caller");
    CHECK(strchr(log, '\r') == NULL, "the captured output carries no raw CR");
    CHECK(exists(dest), "the finished file landed at dest");
    CHECK(!exists(part), "no .part file is left behind");
    CHECK(d.got_bytes == size, "got_bytes matches the source size");
    CHECK(d.out_fd == -1, "the pipe was closed");
    CHECK(download_reap(&d) == 1, "download_reap is idempotent after finishing");
    unlink(dest);

    /* --- failure: nonzero exit leaves nothing behind --- */
    char badurl[512];
    snprintf(badurl, sizeof(badurl), "file://%s/does-not-exist.bin", dir);
    CHECK(download_start(&d, badurl, dest, 0) == 0, "download_start succeeds for a bad url too");
    rc = drive(&d, log, sizeof(log), 20000);
    CHECK(rc != 0, "curl exits nonzero for an unreadable url");
    CHECK(!exists(dest), "a failed download never occupies the final filename");
    CHECK(!exists(part), "the .part file is removed on failure");
    CHECK(strstr(log, "curl:") != NULL, "curl's error message reached the caller");

    /* --- reaping is not the end of reading --- */
    /* Deterministic version of the race above: let curl exit *before* the first
     * read, so its whole message is sitting buffered in the pipe when the child
     * is reaped. A loop that stops reading once reap succeeds drops exactly
     * this -- the error text the child-process design exists to surface. */
    CHECK(download_start(&d, badurl, dest, 0) == 0, "download_start for the reap-first case");
    struct timespec settle = { .tv_nsec = 400 * 1000 * 1000 };
    nanosleep(&settle, NULL);
    CHECK(download_reap(&d) == 1, "reap sees the already-exited child");
    CHECK(d.out_fd >= 0, "the pipe is still open after reaping");
    log[0] = '\0';
    drain_to_eof(&d, log, sizeof(log));
    CHECK(strstr(log, "curl:") != NULL,
          "output buffered in the pipe survives being reaped first");
    CHECK(!exists(part) && !exists(dest), "the reaped failure still cleaned up");

    /* --- cancel: bounded, reaps the child, removes the .part --- */
    if (mkfifo(fifo, 0600) == 0) {
        /* curl blocks opening a FIFO nobody writes to, so the child is reliably
         * still running when we cancel -- with no disk churn. */
        char fifourl[512];
        snprintf(fifourl, sizeof(fifourl), "file://%s", fifo);
        CHECK(download_start(&d, fifourl, dest, 0) == 0, "download_start for the stalled url");

        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        download_cancel(&d);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double secs = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;

        CHECK(secs < 3.0, "download_cancel returns promptly rather than hanging");
        CHECK(!d.running && d.out_fd == -1, "cancel clears running and closes the pipe");
        CHECK(!exists(part) && !exists(dest), "cancel leaves no .part or dest behind");
        CHECK(waitpid(d.pid, NULL, WNOHANG) == -1 && errno == ECHILD,
              "cancel leaves no zombie");
        unlink(fifo);
    } else {
        printf("test_download: note -- mkfifo failed (%s), cancel path not exercised\n",
               strerror(errno));
    }

    /* --- a download that was never started is safe to cancel --- */
    struct download idle;
    memset(&idle, 0, sizeof(idle));
    idle.out_fd = -1;
    download_cancel(&idle);

    char models[256];
    snprintf(models, sizeof(models), "%s/models", dir);
    unlink(src);
    rmdir(models);
    rmdir(dir);

    if (failures) { fprintf(stderr, "test_download: %d failure(s)\n", failures); return 1; }
    printf("test_download: OK\n");
    return 0;
}
