#include "inject_ydotool.h"
#include "log.h"

#include <errno.h>
#include <limits.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#define YDOTOOL_BIN "ydotool"
/* Historical ydotoold default; override via YDOTOOL_SOCKET if the installed
 * package uses a different path (verify against `ydotoold --help` / its
 * packaged service file). */
#define YDOTOOL_DEFAULT_SOCKET "/tmp/.ydotool_socket"

static int find_ydotool_binary(void)
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
        snprintf(candidate, sizeof(candidate), "%s/%s", dir, YDOTOOL_BIN);
        if (access(candidate, X_OK) == 0) {
            found = 0;
            break;
        }
    }
    free(path_copy);
    return found;
}

/* Resolves the same way the `ydotool` CLI itself does: an explicit
 * YDOTOOL_SOCKET wins; otherwise $XDG_RUNTIME_DIR/.ydotool_socket (this is
 * what a `ydotoold` started in a normal user session defaults to); only
 * falls back to a fixed /tmp path if XDG_RUNTIME_DIR isn't set (e.g. a
 * ydotoold started via `sudo` without -E, which has no XDG_RUNTIME_DIR of
 * its own). Buffer is static -- fine for this single-threaded lookup. */
static const char *ydotool_socket_path(void)
{
    const char *sock_path = getenv("YDOTOOL_SOCKET");
    if (sock_path && sock_path[0] != '\0')
        return sock_path;

    const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
    if (runtime_dir && runtime_dir[0] != '\0') {
        static char buf[PATH_MAX];
        snprintf(buf, sizeof(buf), "%s/.ydotool_socket", runtime_dir);
        return buf;
    }

    return YDOTOOL_DEFAULT_SOCKET;
}

static int check_daemon_socket(void)
{
    /* ydotoold's IPC socket is SOCK_DGRAM (confirmed empirically -- SOCK_STREAM
     * connect() fails with EPROTOTYPE even against a live daemon). */
    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", ydotool_socket_path());

    int rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    close(fd);
    return rc == 0 ? 0 : -1;
}

int inject_ydotool_check(void)
{
    if (find_ydotool_binary() != 0) {
        log_error("inject_ydotool: 'ydotool' binary not found on PATH -- install it "
                  "(Arch: pacman -S ydotool; Ubuntu: apt install ydotool, verify it's "
                  "packaged for your release)");
        return -1;
    }

    if (check_daemon_socket() != 0) {
        log_error("inject_ydotool: cannot reach ydotoold at '%s' -- start the daemon "
                  "(e.g. `ydotoold &` or enable its service) and make sure it has access "
                  "to /dev/uinput (root, or a udev rule granting your group access)",
                  ydotool_socket_path());
        return -1;
    }

    return 0;
}

/* Runs `ydotool <argv...>` via fork+execvp (never a shell), waits for it, and
 * logs+returns -1 on spawn failure or nonzero exit. `argv` must be NULL-terminated
 * and its [0] should be YDOTOOL_BIN. */
static int run_ydotool(char *const argv[])
{
    pid_t pid = fork();
    if (pid < 0) {
        log_warn("inject_ydotool: fork failed: %s", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        execvp(YDOTOOL_BIN, argv);
        _exit(127); /* execvp only returns on failure */
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        log_warn("inject_ydotool: waitpid failed: %s", strerror(errno));
        return -1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        log_warn("inject_ydotool: 'ydotool' exited abnormally (status %d)",
                  WIFEXITED(status) ? WEXITSTATUS(status) : status);
        return -1;
    }
    return 0;
}

static int type_run(char *run)
{
    if (run[0] == '\0')
        return 0;
    char *argv[] = { (char *)YDOTOOL_BIN, "type", "--", run, NULL };
    return run_ydotool(argv);
}

static int send_key(int keycode)
{
    char down[16], up[16];
    snprintf(down, sizeof(down), "%d:1", keycode);
    snprintf(up, sizeof(up), "%d:0", keycode);
    char *argv[] = { (char *)YDOTOOL_BIN, "key", down, up, NULL };
    return run_ydotool(argv);
}

int inject_ydotool_type_text(const char *text)
{
    if (!text)
        return -1;

    /* A single contiguous run can be at most strlen(text) bytes. */
    char *run = malloc(strlen(text) + 1);
    if (!run) {
        log_error("inject_ydotool: out of memory");
        return -1;
    }

    size_t run_len = 0;
    for (const char *p = text; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '\n' || c == '\t') {
            run[run_len] = '\0';
            type_run(run);
            run_len = 0;
            send_key(c == '\n' ? KEY_ENTER : KEY_TAB);
        } else if (c >= 0x20 && c <= 0x7E) {
            run[run_len++] = (char)c;
        } else {
            run[run_len] = '\0';
            type_run(run);
            run_len = 0;
            log_warn("inject_ydotool: skipping unmappable byte 0x%02x", c);
        }
    }
    run[run_len] = '\0';
    type_run(run);

    free(run);
    return 0;
}
