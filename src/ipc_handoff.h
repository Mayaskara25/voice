#ifndef DICTATION_IPC_HANDOFF_H
#define DICTATION_IPC_HANDOFF_H

#include "app_state.h"

#include <pthread.h>
#include <stddef.h>

/* Worker-thread -> GUI-thread handoff.
 *
 * The GUI thread blocks in poll() over the X connection fd AND the read end of
 * a self-pipe owned by this struct. A plain shared flag cannot wake a thread
 * parked in poll()/XNextEvent, so every state change the worker publishes also
 * writes one byte to the pipe (see PLAN.md: "A shared-memory flag alone cannot
 * wake a thread blocked in XNextEvent/select()").
 *
 * Threading contract:
 *   - Worker thread calls ipc_set_state() / ipc_post_inject() only.
 *   - GUI thread calls ipc_drain() / ipc_get_state() / ipc_get_transcript() /
 *     ipc_take_inject_text() only, and actually performs the injection (it owns
 *     the X Display). See PLAN.md forward-compatibility contract. */
struct ipc_handoff {
    pthread_mutex_t lock;
    int   pipe_fds[2];           /* [0] read end (GUI polls), [1] write end (worker signals) */
    enum  app_state state;
    char *pending_inject_text;   /* malloc'd; GUI takes ownership, injects, frees. NULL = none. */
    char  last_transcript[512];  /* copy of the most recent text, for display */
};

/* Creates the self-pipe (both ends non-blocking) and inits the mutex.
 * Returns 0 on success, -1 on error (message logged). */
int  ipc_init(struct ipc_handoff *h);
void ipc_free(struct ipc_handoff *h);

/* The fd the GUI thread must add to its poll() set. */
int  ipc_wakeup_fd(struct ipc_handoff *h);

/* ---- worker side ---- */

/* Publishes a new pipeline state and wakes the GUI thread. */
void ipc_set_state(struct ipc_handoff *h, enum app_state s);

/* Hands the final text off to the GUI thread to inject: stores a copy for
 * display, takes ownership of `text` as the pending injection, sets state to
 * INJECTING, and wakes the GUI thread. `text` must be malloc'd; ownership
 * transfers to the handoff (freed later by ipc_take_inject_text's caller). */
void ipc_post_inject(struct ipc_handoff *h, char *text);

/* ---- GUI side ---- */

/* Reads and discards all pending wakeup bytes from the pipe. */
void ipc_drain(struct ipc_handoff *h);

enum app_state ipc_get_state(struct ipc_handoff *h);

/* Copies the last transcript into out (always NUL-terminated). */
void ipc_get_transcript(struct ipc_handoff *h, char *out, size_t out_size);

/* Returns the pending injection text and clears it (caller must free), or NULL
 * if nothing is pending. */
char *ipc_take_inject_text(struct ipc_handoff *h);

#endif
