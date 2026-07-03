#include "ipc_handoff.h"
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int ipc_init(struct ipc_handoff *h)
{
    memset(h, 0, sizeof(*h));
    h->state = APP_STATE_IDLE;
    h->pending_inject_text = NULL;

    if (pipe(h->pipe_fds) != 0) {
        log_error("ipc: pipe() failed: %s", strerror(errno));
        return -1;
    }
    /* Both ends non-blocking: the worker must never stall on a write if the
     * GUI is momentarily behind (a dropped wakeup byte is harmless -- the state
     * lives in shared memory and is picked up on the next drain), and the GUI's
     * drain reads until EAGAIN. */
    if (set_nonblock(h->pipe_fds[0]) != 0 || set_nonblock(h->pipe_fds[1]) != 0) {
        log_error("ipc: fcntl(O_NONBLOCK) failed: %s", strerror(errno));
        close(h->pipe_fds[0]);
        close(h->pipe_fds[1]);
        return -1;
    }

    if (pthread_mutex_init(&h->lock, NULL) != 0) {
        log_error("ipc: pthread_mutex_init failed");
        close(h->pipe_fds[0]);
        close(h->pipe_fds[1]);
        return -1;
    }
    return 0;
}

void ipc_free(struct ipc_handoff *h)
{
    if (h->pipe_fds[0] >= 0)
        close(h->pipe_fds[0]);
    if (h->pipe_fds[1] >= 0)
        close(h->pipe_fds[1]);
    h->pipe_fds[0] = h->pipe_fds[1] = -1;
    free(h->pending_inject_text);
    h->pending_inject_text = NULL;
    pthread_mutex_destroy(&h->lock);
}

int ipc_wakeup_fd(struct ipc_handoff *h)
{
    return h->pipe_fds[0];
}

/* Writes one wakeup byte to the pipe. EAGAIN (pipe full) is fine -- see
 * ipc_init. Not called under the lock's critical assumptions beyond ordering. */
static void signal_pipe(struct ipc_handoff *h)
{
    const unsigned char byte = 1;
    ssize_t n;
    do {
        n = write(h->pipe_fds[1], &byte, 1);
    } while (n < 0 && errno == EINTR);
}

void ipc_set_state(struct ipc_handoff *h, enum app_state s)
{
    pthread_mutex_lock(&h->lock);
    h->state = s;
    pthread_mutex_unlock(&h->lock);
    signal_pipe(h);
}

void ipc_post_inject(struct ipc_handoff *h, char *text)
{
    pthread_mutex_lock(&h->lock);
    if (text) {
        snprintf(h->last_transcript, sizeof(h->last_transcript), "%s", text);
    }
    /* If a previous injection was never taken, don't leak it. */
    free(h->pending_inject_text);
    h->pending_inject_text = text;
    h->state = APP_STATE_INJECTING;
    pthread_mutex_unlock(&h->lock);
    signal_pipe(h);
}

void ipc_drain(struct ipc_handoff *h)
{
    unsigned char buf[64];
    ssize_t n;
    do {
        n = read(h->pipe_fds[0], buf, sizeof(buf));
    } while (n > 0 || (n < 0 && errno == EINTR));
    /* n < 0 with EAGAIN => drained. */
}

enum app_state ipc_get_state(struct ipc_handoff *h)
{
    pthread_mutex_lock(&h->lock);
    enum app_state s = h->state;
    pthread_mutex_unlock(&h->lock);
    return s;
}

void ipc_get_transcript(struct ipc_handoff *h, char *out, size_t out_size)
{
    if (out_size == 0)
        return;
    pthread_mutex_lock(&h->lock);
    snprintf(out, out_size, "%s", h->last_transcript);
    pthread_mutex_unlock(&h->lock);
}

char *ipc_take_inject_text(struct ipc_handoff *h)
{
    pthread_mutex_lock(&h->lock);
    char *text = h->pending_inject_text;
    h->pending_inject_text = NULL;
    pthread_mutex_unlock(&h->lock);
    return text;
}
