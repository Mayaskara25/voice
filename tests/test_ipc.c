/* test_ipc: verifies the worker->GUI handoff -- crucially that a byte written
 * on one thread reliably wakes a poll() on another (the self-pipe mechanic that
 * a plain shared flag cannot provide), plus state/inject-text transfer. */
#include "app_state.h"
#include "ipc_handoff.h"

#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
} while (0)

static void *worker(void *p)
{
    struct ipc_handoff *h = p;
    struct timespec ts = { 0, 100 * 1000 * 1000 }; /* 100 ms */
    nanosleep(&ts, NULL);
    ipc_set_state(h, APP_STATE_TRANSCRIBING);
    return NULL;
}

int main(void)
{
    struct ipc_handoff h;
    CHECK(ipc_init(&h) == 0, "ipc_init succeeds");

    pthread_t th;
    pthread_create(&th, NULL, worker, &h);

    struct pollfd pfd = { .fd = ipc_wakeup_fd(&h), .events = POLLIN };
    int r = poll(&pfd, 1, 2000); /* must wake well before 2s */
    CHECK(r == 1, "poll() woke from the cross-thread self-pipe signal");
    ipc_drain(&h);
    CHECK(ipc_get_state(&h) == APP_STATE_TRANSCRIBING, "state published across threads");

    pthread_join(th, NULL);

    /* inject handoff: post -> INJECTING + stored transcript -> take (once) */
    char *msg = strdup("hello world");
    ipc_post_inject(&h, msg);
    CHECK(ipc_get_state(&h) == APP_STATE_INJECTING, "ipc_post_inject sets INJECTING");
    char tr[64];
    ipc_get_transcript(&h, tr, sizeof(tr));
    CHECK(strcmp(tr, "hello world") == 0, "transcript copied for display");
    char *taken = ipc_take_inject_text(&h);
    CHECK(taken != NULL && strcmp(taken, "hello world") == 0, "take returns the posted text");
    free(taken);
    CHECK(ipc_take_inject_text(&h) == NULL, "second take returns NULL (ownership moved)");

    ipc_free(&h);

    if (failures) { fprintf(stderr, "test_ipc: %d failure(s)\n", failures); return 1; }
    printf("test_ipc: OK\n");
    return 0;
}
