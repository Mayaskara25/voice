/* pipe2() is a GNU extension (POSIX only has pipe()); we want it so the pipe is
 * created O_CLOEXEC atomically rather than via a follow-up fcntl. */
#define _GNU_SOURCE

#include "downloader.h"
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CURL_BIN "curl"

/* Same PATH walk as inject_ydotool.c's find_ydotool_binary(). */
static int find_curl_binary(void)
{
    const char *path_env = getenv("PATH");
    if (!path_env)
        path_env = "/usr/local/bin:/usr/bin:/bin";

    char *path_copy = strdup(path_env);
    if (!path_copy)
        return -1;

    int found = -1;
    char *saveptr = NULL;
    for (char *dir = strtok_r(path_copy, ":", &saveptr); dir; dir = strtok_r(NULL, ":", &saveptr)) {
        char candidate[PATH_MAX];
        snprintf(candidate, sizeof(candidate), "%s/%s", dir, CURL_BIN);
        if (access(candidate, X_OK) == 0) {
            found = 0;
            break;
        }
    }
    free(path_copy);
    return found;
}

int download_check_curl(void)
{
    if (find_curl_binary() != 0) {
        log_error("downloader: 'curl' binary not found on PATH -- install it "
                  "(Arch: pacman -S curl; Debian/Ubuntu: apt install curl). It is only "
                  "needed to fetch models; the dictation daemon itself does no networking");
        return -1;
    }
    return 0;
}

/* Creates every missing component of `path`'s parent directory. A fresh clone
 * has no models/ at all -- git doesn't track empty directories -- and curl
 * would otherwise fail with a bare "Failed to open the file". */
static int make_parent_dirs(const char *path)
{
    char buf[CONFIG_PATH_MAX];
    int n = snprintf(buf, sizeof(buf), "%s", path);
    if (n < 0 || (size_t)n >= sizeof(buf))
        return -1;

    char *slash = strrchr(buf, '/');
    if (!slash)
        return 0;               /* writing into the cwd */
    *slash = '\0';
    if (buf[0] == '\0')
        return 0;               /* "/file" -- the root always exists */

    for (char *p = buf + 1; ; p++) {
        if (*p != '/' && *p != '\0')
            continue;
        char saved = *p;
        *p = '\0';
        if (mkdir(buf, 0755) != 0 && errno != EEXIST) {
            log_error("downloader: could not create directory '%s': %s", buf, strerror(errno));
            return -1;
        }
        *p = saved;
        if (saved == '\0')
            break;
    }
    return 0;
}

int download_start(struct download *d, const char *url, const char *dest, long expected)
{
    memset(d, 0, sizeof(*d));
    d->out_fd = -1;
    d->at_line_start = 1;
    d->expected_bytes = expected > 0 ? expected : 0;

    int n = snprintf(d->dest, sizeof(d->dest), "%s", dest);
    if (n < 0 || (size_t)n >= sizeof(d->dest)) {
        log_error("downloader: destination path too long: %s", dest);
        return -1;
    }
    n = snprintf(d->part, sizeof(d->part), "%s.part", dest);
    if (n < 0 || (size_t)n >= sizeof(d->part)) {
        log_error("downloader: destination path too long for a .part sibling: %s", dest);
        return -1;
    }
    n = snprintf(d->cmdline, sizeof(d->cmdline), "%s -L --fail -o %s %s",
                 CURL_BIN, d->part, url);
    if (n < 0 || (size_t)n >= sizeof(d->cmdline)) {
        log_error("downloader: url too long: %s", url);
        return -1;
    }

    if (make_parent_dirs(d->part) != 0)
        return -1;

    /* O_CLOEXEC on both ends so an exec elsewhere can't leak them; dup2 in the
     * child clears it on the descriptors we deliberately keep. */
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0) {
        log_error("downloader: pipe2 failed: %s", strerror(errno));
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        log_error("downloader: fork failed: %s", strerror(errno));
        close(fds[0]);
        close(fds[1]);
        return -1;
    }

    if (pid == 0) {
        /* Child: curl's stdout (the meter) and stderr (its errors) both go down
         * the one pipe, so the log pane shows exactly what a terminal would. */
        close(fds[0]);
        if (dup2(fds[1], STDOUT_FILENO) < 0 || dup2(fds[1], STDERR_FILENO) < 0)
            _exit(127);
        if (fds[1] != STDOUT_FILENO && fds[1] != STDERR_FILENO)
            close(fds[1]);

        /* These point into the child's own copy-on-write copy of *d, not the
         * parent's -- fork duplicated the pages. Nothing here may be changed to
         * assume the parent's struct stays alive or unmoved. */
        char *const argv[] = {
            (char *)CURL_BIN, (char *)"-L", (char *)"--fail",
            (char *)"-o", d->part, (char *)url, NULL
        };
        execvp(CURL_BIN, argv);
        _exit(127);             /* only reached if exec failed */
    }

    close(fds[1]);
    /* Non-blocking read end: download_read() must never stall the caller's
     * render loop, however little curl has produced. */
    int flags = fcntl(fds[0], F_GETFL, 0);
    if (flags < 0 || fcntl(fds[0], F_SETFL, flags | O_NONBLOCK) != 0)
        log_warn("downloader: could not set the pipe non-blocking: %s", strerror(errno));

    d->pid = pid;
    d->out_fd = fds[0];
    d->running = 1;
    d->exit_code = -1;
    return 0;
}

size_t download_translate_cr(char *buf, size_t n, int *at_line_start)
{
    size_t out = 0;
    for (size_t i = 0; i < n; i++) {
        char c = buf[i] == '\r' ? '\n' : buf[i];
        if (c == '\n') {
            if (*at_line_start)
                continue;       /* collapse "\r\n", "\r\r", and blank runs */
            *at_line_start = 1;
        } else {
            *at_line_start = 0;
        }
        buf[out++] = c;
    }
    return out;
}

int download_read(struct download *d, char *buf, size_t n)
{
    if (d->out_fd < 0 || n == 0)
        return -1;

    ssize_t got;
    do {
        got = read(d->out_fd, buf, n);
    } while (got < 0 && errno == EINTR);

    if (got > 0)
        return (int)download_translate_cr(buf, (size_t)got, &d->at_line_start);

    if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        return 0;

    if (got < 0)
        log_warn("downloader: read failed: %s", strerror(errno));

    close(d->out_fd);
    d->out_fd = -1;
    return -1;
}

void download_update_progress(struct download *d)
{
    struct stat st;
    if (stat(d->part, &st) == 0)
        d->got_bytes = (long)st.st_size;
}

/* rename(part, dest) on success, unlink(part) on failure -- so a partial or
 * failed download never occupies the final filename, where it would fail later
 * inside whisper.cpp instead of here. */
static void finalize(struct download *d)
{
    struct stat st;
    int have_part = (stat(d->part, &st) == 0);

    if (d->exit_code != 0) {
        if (have_part) {
            unlink(d->part);
            log_warn("downloader: exit %d -- removed the partial file '%s'",
                     d->exit_code, d->part);
        } else {
            log_warn("downloader: exit %d -- nothing was written", d->exit_code);
        }
        return;
    }

    if (!have_part) {
        /* Success with no output file: treat as a failure rather than silently
         * reporting a model that isn't there. */
        log_error("downloader: curl exited 0 but '%s' does not exist", d->part);
        d->exit_code = -1;
        return;
    }

    d->got_bytes = (long)st.st_size;
    if (d->expected_bytes > 0 && d->got_bytes != d->expected_bytes) {
        /* Advisory only: catalog sizes go stale when upstream re-uploads. Worth
         * saying out loud, not worth rejecting a file curl considers complete. */
        log_warn("downloader: '%s' is %ld bytes, catalog says %ld -- keeping it "
                 "(the catalog size is advisory)", d->dest, d->got_bytes, d->expected_bytes);
    }

    if (rename(d->part, d->dest) != 0) {
        log_error("downloader: could not rename '%s' -> '%s': %s",
                  d->part, d->dest, strerror(errno));
        d->exit_code = -1;
        return;
    }
    log_info("downloader: saved %s (%ld bytes)", d->dest, d->got_bytes);
}

int download_reap(struct download *d)
{
    if (!d->running)
        return 1;               /* idempotent: already reaped and finalized */

    int status = 0;
    pid_t got;
    do {
        got = waitpid(d->pid, &status, WNOHANG);
    } while (got < 0 && errno == EINTR);

    if (got == 0)
        return 0;
    if (got < 0) {
        log_error("downloader: waitpid failed: %s", strerror(errno));
        d->running = 0;
        d->exit_code = -1;
        return -1;
    }

    d->running = 0;
    if (WIFEXITED(status))
        d->exit_code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status))
        d->exit_code = 128 + WTERMSIG(status);
    else
        d->exit_code = -1;

    finalize(d);
    return 1;
}

void download_cancel(struct download *d)
{
    if (d->running) {
        kill(d->pid, SIGTERM);

        /* Bounded wait: a hung teardown would freeze the setup window's close
         * path (and any test that exercises cancellation), so escalate rather
         * than block indefinitely on a curl that ignores SIGTERM. */
        int reaped = 0;
        for (int i = 0; i < 200 && !reaped; i++) {        /* ~2s at 10ms */
            int status;
            pid_t got = waitpid(d->pid, &status, WNOHANG);
            if (got == d->pid)
                reaped = 1;
            else if (got < 0 && errno != EINTR)
                reaped = 1;                              /* nothing left to wait for */
            else
                nanosleep(&(struct timespec){ .tv_nsec = 10 * 1000 * 1000 }, NULL);
        }
        if (!reaped) {
            log_warn("downloader: curl ignored SIGTERM, sending SIGKILL");
            kill(d->pid, SIGKILL);
            int status;
            while (waitpid(d->pid, &status, 0) < 0 && errno == EINTR)
                ;
        }

        d->running = 0;
        d->exit_code = 130;     /* conventional "terminated by the user" */
    }

    if (d->out_fd >= 0) {
        close(d->out_fd);
        d->out_fd = -1;
    }
    if (d->part[0] != '\0')
        unlink(d->part);
}
