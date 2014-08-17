/***
 *
 * Independently contributed to the lwlock library. This file is also released under the
 * terms of MPL 2.0 license but you do not need to sign the committer agreement for
 * this file.
 */

#include "lw_bitlock.h"
#include "lw_debug.h"
#include "lw_atomic.h"
#include "delegate.h"
#include <errno.h>

typedef struct {
    delegated_job_t      *job_to_cancel;
    int                 cancel_result;
} delegate_cancel_arg_t;

static delegate_job_status_t delegate_cancel_job(delegate_t *delegate,
                                                 void *arg,
                                                 lw_uint64_t *updated_state);
static delegate_job_status_t job_to_unblock_another_job(delegate_t *delegate,
                                                        void *arg,
                                                        lw_uint64_t *updated_state);

/**
 * Set the specified bits to 1. The bits argument on return will have the current state before
 * the set was done.
 *
 * @param delegate (i/o) the delegate whose state is to be set.
 * @param bits (i/o) has bits to set on input. On return has current state of delegate (before
 * the set).
 */
static void
delegate_set_bits(delegate_t *delegate, delegate_state_t *bits)
{
    delegate_state_t old, new;
    delegate_state_t *state = &delegate->state;

    lw_assert(bits->fields.unused == 0);
    lw_assert(bits->atomic64 != 0); // at least 1 bit is being set.

    old.atomic64 = state->atomic64;
    do {
        new.atomic64 = old.atomic64 | bits->atomic64;
    } while (!lw_uint64_swap(&state->atomic64, &old.atomic64, new.atomic64));

    lw_assert(old.fields.unused == new.fields.unused);
    bits->atomic64 = old.atomic64;
    return;
}

/**
 * Like delegate_set_bits but clears the bits.
 */
static void
delegate_clear_bits(delegate_t *delegate, delegate_state_t *bits)
{
    delegate_state_t old, new;
    delegate_state_t *state = &delegate->state;

    lw_assert(bits->fields.unused == 0);
    lw_assert(bits->atomic64 != 0); // at least 1 bit is being set.

    old.atomic64 = state->atomic64;
    do {
        new.atomic64 = old.atomic64 & ~bits->atomic64;
    } while (!lw_uint64_swap(&state->atomic64, &old.atomic64, new.atomic64));

    lw_assert(old.fields.unused == new.fields.unused);
    bits->atomic64 = old.atomic64;
    return;
}

static lw_bool_t
delegate_try_lock(delegate_t *delegate)
{
    delegate_state_t old;

    old.atomic64 = 0;
    old.fields.lock = 1;
    delegate_set_bits(delegate, &old);
    return old.fields.lock == 0;
}

/**
 * Push a job to the incoming stack and then set the incoming bit.
 */
static void
push_incoming(delegate_t *delegate, delegated_job_t *job, delegate_state_t *change)
{
    lw_uint64_t old;

    lw_assert(change->fields.unused == 0);

    old = (uintptr_t)delegate->incoming_stack;
    do {
        job->event.iface.link.next = (lw_delem_t *)delegate->incoming_stack;
    } while (!lw_uint64_swap((volatile lw_uint64_t *)&delegate->incoming_stack,
                              &old, (lw_uint64_t)job));

    change->fields.incoming = 1;
    delegate_set_bits(delegate, change);
}

/**
 * clear the incoming bit and then pop everything off the incoming stack and drain to
 * queued_jobs list. The order of clearing the bit and popping the stack is deliberately
 * opposite that of the push_incoming to avoid any race leaving dangling jobs.
 */
static void
drain_incoming(delegate_t *delegate, delegate_state_t *old)
{
    delegated_job_t *job, *last_inserted;
    delegate_state_t change;
    lw_dlist_t immediate_reqs;
    lw_delem_t *elem;

    lw_assert(delegate->state.fields.lock == 1);
    if (delegate->state.fields.incoming == 0) {
        /* Nothing to drain. */
        return;
    }

    lw_dl_init(&immediate_reqs);

    change.atomic64 = 0;
    change.fields.incoming = 1;
    delegate_clear_bits(delegate, &change);

    job = delegate->incoming_stack;
    do {
        /* Nothing more here. */
    } while (!lw_uint64_swap((volatile lw_uint64_t *)&delegate->incoming_stack,
                             (volatile lw_uint64_t *)&job, (lw_uint64_t)NULL));

    last_inserted = NULL;
    while (job != NULL) {
        delegated_job_t *next_to_process = (delegated_job_t *)job->event.iface.link.next;
        ASSERT_ELEM_OFF_LIST(&job->event.iface.link);
        lw_dl_init_elem(&job->event.iface.link);
        if (job->func == delegate_cancel_job ||
            job->func == job_to_unblock_another_job) {
            /*
             * Special processing for cancel jobs to improve response latency and
             * actually be able to cancel the job in the first place. Same for
             * unblocking.
             */
            lw_dl_append_at_end(&immediate_reqs, &job->event.iface.link);
            job = next_to_process;
            continue;
        }
        if (last_inserted == NULL) {
            lw_dl_append_at_end(&delegate->queued_jobs, &job->event.iface.link);
        } else {
            lw_dl_insert_before(&delegate->queued_jobs, &last_inserted->event.iface.link,
                                &job->event.iface.link);
        }
        lw_assert(lw_dl_elem_is_in_list(&delegate->queued_jobs, &job->event.iface.link));
        last_inserted = job;
        job = next_to_process;
    }

    while ((elem = lw_dl_dequeue(&immediate_reqs)) != NULL) {
        job = LW_FIELD_2_OBJ(elem, *job, event.iface.link);
        delegate_job_status_t status = job->func(delegate, job->arg, &old->atomic64);
        lw_verify(status == DELEGATED_DONE);
        lw_event_signal(&job->event, delegate);
    }
}

/**
 * Run all the queued jobs one at a time.
 */
static void
run_queued_jobs(delegate_t *delegate, delegate_state_t *old)
{
    delegated_job_t *job_to_run;
    delegate_job_status_t status;

    while (delegate->queued_jobs.head != NULL) {
        job_to_run = LW_FIELD_2_OBJ(delegate->queued_jobs.head, *job_to_run, event.iface.link);

        lw_assert(!job_to_run->is_blocked && job_to_run->delegate == delegate);;
        status = job_to_run->func(delegate, job_to_run->arg, &old->atomic64);

        switch (status) {
            case DELEGATED_DONE:
                /* Job done. */
                LW_IGNORE_RETURN_VALUE(lw_dl_dequeue(&delegate->queued_jobs));
                job_to_run->delegate = NULL;
                lw_event_signal(&job_to_run->event, delegate);
                break;
            case DELEGATED_YIELD:
                LW_IGNORE_RETURN_VALUE(lw_dl_dequeue(&delegate->queued_jobs));
                lw_dl_append_at_end(&delegate->queued_jobs, &job_to_run->event.iface.link);
                break;
            case DELEGATED_BLOCK_ALL:
                /* Means that processing should be stopped and resumed later. */
                return;
            case DELEGATED_BLOCK_THIS:
                job_to_run->is_blocked = TRUE;
                LW_IGNORE_RETURN_VALUE(lw_dl_dequeue(&delegate->queued_jobs));
                lw_dl_append_at_end(&delegate->blocked_jobs, &job_to_run->event.iface.link);
                break;
            case DELEGATED_RELOAD:
                break; // Just loop again.
            default:
                lw_verify(FALSE);
                break;
        }
        lw_dl_assert_count(&delegate->queued_jobs);
    }
}

/**
 * Main loop for delegate. It runs functions, drains incoming and runs some more until it
 * is all done.
 */
void
delegate_run_internal(delegate_t *delegate)
{
    delegate_state_t old, new;
    delegate_state_t *state = &delegate->state;

    old.atomic64 = state->atomic64;
    lw_assert(old.fields.lock == 1);
    do {
        drain_incoming(delegate, &old);
        old.fields.incoming = 0;

        run_queued_jobs(delegate, &old);
        new.atomic64 = old.atomic64;
        new.fields.pending = lw_dl_get_count(&delegate->queued_jobs) > 0;
        if (old.fields.waiting == 0) {
            new.fields.lock = 0;
        }
    } while (!lw_uint64_swap(&state->atomic64, &old.atomic64, new.atomic64));

    if (old.fields.waiting) {
        /* Lock is still held. Some other thread must have asked to take over. */
        delegate_state_t lock_mask, wait_mask;
        lock_mask.atomic64 = wait_mask.atomic64 = 0;
        lock_mask.fields.lock = 1;
        wait_mask.fields.waiting = 1;

        lw_verify(new.fields.lock == 1);
        lw_bitlock64_unlock(&state->atomic64, lock_mask.atomic64, wait_mask.atomic64);
    }
}

/**
 * External API for running delegated jobs. If the delegator is currently locked, then
 * this thread will wait until the lock is handed over to it.
 */
void
delegate_run(delegate_t *delegate, lw_bool_t no_wait)
{
    delegate_state_t lock_mask, wait_mask;
    lock_mask.atomic64 = wait_mask.atomic64 = 0;
    lock_mask.fields.lock = 1;
    wait_mask.fields.waiting = 1;

    if (no_wait) {
        if (!delegate_try_lock(delegate)) {
            return;
        }
    } else {
        lw_bitlock64_lock(&delegate->state.atomic64, lock_mask.atomic64, wait_mask.atomic64);
    }
    delegate_run_internal(delegate); // lock dropped in here.
}

/**
 * Delegate a job. Attempt to run it immediately if run_now is true.
 */
void
delegate_submit(delegate_t *delegate, delegated_job_t *job, lw_bool_t run_now)
{
    delegate_state_t old, new;
    delegate_state_t *state = &delegate->state;
    lw_bool_t fast_path;

    job->delegate = delegate;
    job->is_blocked = FALSE;

    if (!run_now) {
        old.atomic64 = 0;
        push_incoming(delegate, job, &old);
        return;
    }

    old.atomic64 = state->atomic64;
    do {
        if (old.fields.incoming | old.fields.pending | old.fields.lock) {
            fast_path = FALSE;
            break;
        }
        new.atomic64 = old.atomic64;
        new.fields.lock = 1;
        fast_path = TRUE;
    } while (!lw_uint64_swap(&state->atomic64, &old.atomic64, new.atomic64));

    if (fast_path) {
        lw_assert(lw_dl_get_count(&delegate->queued_jobs) == 0);
        lw_dl_append_at_end(&delegate->queued_jobs, &job->event.iface.link);
        delegate_run_internal(delegate);
        return;
    }
    old.atomic64 = 0;
    old.fields.lock = 1;
    push_incoming(delegate, job, &old);
    if (old.fields.lock == 0) {
        /* Acquired lock. */
        lw_assert(delegate->state.fields.lock == 1);
        delegate_run_internal(delegate);
    }
    return;
}

static lw_bool_t
job_still_with_delegate(delegate_t *delegate, delegated_job_t *job, lw_bool_t remove)
{
    lw_dlist_t *list;

    lw_assert(delegate->state.fields.lock == 1);

    if (job->delegate != delegate) {
        lw_assert(!lw_dl_elem_is_in_list(&delegate->blocked_jobs, &job->event.iface.link));
        lw_assert(!lw_dl_elem_is_in_list(&delegate->queued_jobs, &job->event.iface.link));
        return FALSE;
    }

    if (job->is_blocked) {
        list = &delegate->blocked_jobs;
    } else {
        list = &delegate->queued_jobs;
    }

    if (lw_dl_elem_is_in_list(list, &job->event.iface.link)) {
        if (remove) {
            lw_dl_remove(list, &job->event.iface.link);
        }
        return TRUE;
    }
    ASSERT_ELEM_OFF_LIST(&job->event.iface.link);
    return FALSE;
}

static delegate_job_status_t
delegate_cancel_job(delegate_t *delegate, void *arg, lw_uint64_t *updated_state)
{
    delegate_cancel_arg_t *cancel_arg = (delegate_cancel_arg_t *)arg;

    lw_assert(delegate->state.fields.lock == 1);

    if (job_still_with_delegate(delegate, cancel_arg->job_to_cancel, TRUE)) {
        cancel_arg->cancel_result = 0;
    } else {
        cancel_arg->cancel_result = EALREADY;
    }
    return DELEGATED_DONE;
}

/**
 * Cancel a previously added job. The checks for whether the job still exists or is valid are tricky to do
 * reliably in the face of a job already finishing and if the memory for it came from stack, then having
 * unreliable state in it. So it uses the slow search method to ensure the job is still present. Callers
 * should know that this call is not cheap.
 *
 * The function will not block unless the wait flag is specified.
 * The return codes are as follows:
 *
 * 0 - success, job cancelled.
 * EALREADY -- job already over or was never submitted in the first place.
 * EWOULDBLOCK -- was asked to not wait but cannot do the operation without it.
 */
int
delegate_cancel(delegate_t *delegate, delegated_job_t *job, lw_bool_t wait)
{
    delegated_job_t cancel_job;
    delegate_cancel_arg_t cancel_args;
    lw_waiter_domain_t *domain = lw_waiter_global_domain;
    lw_uint8_t  waiter_buf[domain->waiter_size];
    lw_waiter_t *waiter = (lw_waiter_t *)waiter_buf;

    if (delegate_try_lock(delegate)) {
        int ret;
        drain_incoming(delegate, NULL);
        if (job_still_with_delegate(delegate, job, TRUE)) {
            ret = 0;
        } else {
            ret = EALREADY;
        }
        delegate_run_internal(delegate); // Will release lock.
        return ret;
    } else if (!wait) {
        return EWOULDBLOCK;
    }

    cancel_args.job_to_cancel = job;
    cancel_job.func = delegate_cancel_job;
    cancel_job.arg = &cancel_args;
    domain->waiter_event_init(domain, waiter);
    lw_waiter_set_src(waiter, delegate);
    delegate_job_event_default_init(&cancel_job);
    delegate_submit(delegate, &cancel_job, TRUE);
    lw_event_wait(&cancel_job.event, delegate);
    lw_waiter_clear_src(waiter);
    domain->waiter_event_destroy(domain, waiter);
    return cancel_args.cancel_result;
}

static delegate_job_status_t
job_to_unblock_another_job(delegate_t *delegate, void *arg, lw_uint64_t *updated_state)
{
    delegated_job_t *to_unblock = (delegated_job_t *)arg;

    lw_assert(delegate->state.fields.lock == 1);
    lw_verify(to_unblock->delegate == delegate && to_unblock->is_blocked);

    lw_assert(lw_dl_elem_is_in_list(&delegate->blocked_jobs, &to_unblock->event.iface.link));
    lw_dl_remove(&delegate->blocked_jobs, &to_unblock->event.iface.link);
    to_unblock->is_blocked = FALSE;
    lw_dl_append_at_end(&delegate->queued_jobs, &to_unblock->event.iface.link);

    return DELEGATED_DONE;
}

/**
 * Unblock a job that previous ran and blocked itself. The caller is responsible for ensuring that unblock is
 * called for a blocked job and that the job is not cancelled in between.
 */
void
delegate_unblock_job(delegate_t *delegate, delegated_job_t *job)
{
    delegated_job_t unblock_job;
    lw_waiter_domain_t *domain = lw_waiter_global_domain;
    lw_uint8_t  waiter_buf[domain->waiter_size];
    lw_waiter_t *waiter = (lw_waiter_t *)waiter_buf;

    if (delegate_try_lock(delegate)) {
        delegate_job_status_t status = job_to_unblock_another_job(delegate, job, NULL);
        lw_verify(status == DELEGATED_DONE);
        delegate_run_internal(delegate); // Will release lock.
        return;
    }

    unblock_job.arg = job;
    unblock_job.func = job_to_unblock_another_job;
    domain->waiter_event_init(domain, waiter);
    lw_waiter_set_src(waiter, delegate);
    delegate_job_event_default_init(&unblock_job);
    delegate_submit(delegate, &unblock_job, TRUE);
    lw_event_wait(&unblock_job.event, delegate);
    lw_waiter_clear_src(waiter);
    domain->waiter_event_destroy(domain, waiter);
    return;
}

void
delegate_init(delegate_t *delegate)
{
    delegate->state.atomic64 = 0;
    delegate->incoming_stack = NULL;
    lw_dl_init(&delegate->queued_jobs);
    lw_dl_init(&delegate->blocked_jobs);
}

void
delegate_deinit(delegate_t *delegate)
{
    lw_verify(delegate->state.fields.lock == 0 &&
              delegate->state.fields.incoming == 0 &&
              delegate->state.fields.pending == 0 &&
              delegate->state.fields.waiting == 0);

    lw_verify(delegate->incoming_stack == NULL);
    lw_dl_destroy(&delegate->queued_jobs);
    lw_dl_destroy(&delegate->blocked_jobs);
}

/**
 * Initialize the event in a job to the default one. It uses a on stack waiter when it needs
 * to wait. Callers do not have to use this. It is here for convenience of common use case.
 */
void
delegate_job_event_default_init(delegated_job_t *job)
{
    lw_stack_base_event_init(&job->event);
}
