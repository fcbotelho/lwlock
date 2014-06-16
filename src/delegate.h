/***
 *
 * Independently contributed to the lwlock library. This file is also released under the
 * terms of MPL 2.0 license but you do not need to sign the committer agreement for
 * this file.
 */

#ifndef __DELEGATE_H__
#define __DELEGATE_H__

#include "lw_dlist.h"
#include "lw_event.h"

/*
 * Delegation is a building block for event driven programming. The delegation structure
 * specifies the function that needs to be run and a completion event, if any. The caller
 * tries to run the function itself if the delegation lock is free. Otherwise it queues the
 * work (and waits if necessary) to be done by whichever thread is currently holding the
 * delegation lock.
 *
 * All the delegate functions: submit, cancel, unblock can end up running the delegated jobs as well.
 * The user has to ensure that this doesn't cause any issues due to lock ordering.
 *
 */

typedef enum {
    DELEGATED_DONE = 0,         // Delegated function done. Can signal event.
    DELEGATED_YIELD = 1,        // Not done but can go to back of queue.
    DELEGATED_BLOCK_ALL = 2,    // Stop all processing. User responsible for ensuring delegate processing
                                // is resumed later.
    DELEGATED_BLOCK_THIS = 3,   // Block just this delegated function.
    DELEGATED_RELOAD = 4,       // Special code to say go back to queue and pick 1st func again. Used for
                                // those who want to muck with job list themselves. Use with caution. Not
                                // expected to be used generally.

} delegate_job_status_t;

struct delegate_s;
typedef struct delegate_s delegate_t;

typedef delegate_job_status_t (*delegatedFunc)(delegate_t *delegate, void *arg);

typedef struct {
    delegate_t          *delegate;  // For validation during cancel.
    delegatedFunc       func;       // The func to run.
    void                *arg;       // The arg to func.
    lw_bool_t           is_blocked; // Job is blocked.
    lw_base_event_t     event;      // For linking delegated functions and for signaling delegator.
                                    // Keep last to allow for subclassed events.
} delegated_job_t;

#define DELEGATE_STATE_TYPE_NAME delegate_state_t
#define DELEGATE_OTHER_FIELDS   lw_uint64_t   unused:60;

#include "delegate_state_gen.h"

#undef DELEGATE_STATE_TYPE_NAME
#undef DELEGATE_OTHER_FIELDS

struct delegate_s {
    delegate_state_t    state;
    delegated_job_t     *incoming_stack;
    lw_dlist_t          queued_jobs;
    lw_dlist_t          blocked_jobs;
};

void delegate_init(delegate_t *delegate);
void delegate_deinit(delegate_t *delegate);
void delegate_job_event_default_init(delegated_job_t *job);

void delegate_run(delegate_t *delegate, lw_bool_t no_wait);
void delegate_run_internal(delegate_t *delegate);
void delegate_submit(delegate_t *delegate, delegated_job_t *job, lw_bool_t run_now);
int delegate_cancel(delegate_t *delegate, delegated_job_t *job, lw_bool_t wait);
void delegate_unblock_job(delegate_t *delegate, delegated_job_t *job);

#endif /* __DELEGATE_H__ */
