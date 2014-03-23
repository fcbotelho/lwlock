/***
 * Developed originally at EMC Corporation, this library is released under the
 * MPL 2.0 license.  Please refer to the MPL-2.0 file in the repository for its
 * full description or to http://www.mozilla.org/MPL/2.0/ for the online version.
 *
 * Before contributing to the project one needs to sign the committer agreement
 * available in the "committerAgreement" directory.
 */

#include "lw_cond_var.h"
#include "lw_waiter_intern.h"
#include "lw_debug.h"
#include "lw_atomic.h"
#include "lw_cycles.h"

#include <errno.h>

extern void
lw_condvar_wait(LW_INOUT lw_condvar_t *lwcondvar,
                LW_INOUT void *_mutex,
                LW_IN lw_lock_type_t type)
{
    int ret = lw_condvar_timedwait(lwcondvar, _mutex, type, NULL);
    lw_verify(ret == 0);
}

extern int
lw_condvar_timedwait(LW_INOUT lw_condvar_t *lwcondvar,
                     LW_INOUT void *_mutex,
                     LW_IN lw_lock_type_t type,
                     LW_IN struct timespec *abstime)
{
    lw_waiter_t *waiter;
    lw_waiter_t *existing_waiter;
    int wait_result = 0;

    waiter = lw_waiter_get();
    lw_assert(waiter->event.base.wait_src == NULL);
    lw_assert(waiter->next == LW_WAITER_ID_MAX);
    lw_mutex2b_lock(&lwcondvar->lw_condvar_mutex);
    if (lwcondvar->lw_condvar_waiter_id_list == LW_WAITER_ID_MAX) {
        /* First waiter */
        lwcondvar->lw_condvar_waiter_id_list = waiter->id;
        waiter->prev = LW_WAITER_ID_MAX;
    } else {
        for (existing_waiter = lw_waiter_from_id(lwcondvar->lw_condvar_waiter_id_list);
             existing_waiter->next != LW_WAITER_ID_MAX;
             existing_waiter = lw_waiter_from_id(existing_waiter->next)) {
            /* Get to the last waiter */
        }
        existing_waiter->next = waiter->id;
        waiter->prev = existing_waiter->id;
    }
    waiter->event.base.wait_src = lwcondvar;
    lw_mutex2b_unlock(&lwcondvar->lw_condvar_mutex);
    /* Now drop the mutex and wait */
    lw_lock_common_drop_lock(_mutex, type);
    wait_result = lw_waiter_timedwait(waiter, abstime);
    if (wait_result != 0) {
        lw_assert(abstime != NULL);
        lw_assert(wait_result == ETIMEDOUT);
        lw_bool_t got_signal_while_timing_out = FALSE;
        lw_mutex2b_lock(&lwcondvar->lw_condvar_mutex);
        /* Need to extract the waiter out of the queue if it is still
         * on it.
         */
        if (waiter->prev == LW_WAITER_ID_MAX &&
            waiter->id != lwcondvar->lw_condvar_waiter_id_list) {
            /* Waiter got removed from list already */
            got_signal_while_timing_out = TRUE;
            wait_result = 0;
        } else {
            if (waiter->id == lwcondvar->lw_condvar_waiter_id_list) {
                /* This is still the first waiter */
                lwcondvar->lw_condvar_waiter_id_list = waiter->next;
            }
            lw_waiter_remove_from_id_list(waiter);
            waiter->event.base.wait_src = NULL;
        }
        lw_mutex2b_unlock(&lwcondvar->lw_condvar_mutex);
        if (got_signal_while_timing_out) {
            /* There is a pending signal (or soon will be) for this
             * waiter that has to be consumed. This could in theory take
             * a while if the thread that is going to signal gets switched out
             * but we don't have a choice.
             */
            lw_waiter_wait(waiter);
        }
    }

    /* Re-acquire mutex on being woken up before exiting the function. */
    lw_lock_common_acquire_lock(_mutex, type, waiter);

    return wait_result;

}

extern void
lw_condvar_signal(LW_INOUT lw_condvar_t *lwcondvar)
{
    lw_waiter_t *to_wake_up;
    lw_condvar_t old = *lwcondvar;
    if (old.lw_condvar_waiter_id_list == LW_WAITER_ID_MAX) {
        /* Nothing to signal. */
        return;
    }
    lw_mutex2b_lock(&lwcondvar->lw_condvar_mutex);
    to_wake_up = lw_waiter_from_id(lwcondvar->lw_condvar_waiter_id_list);
    if (to_wake_up != NULL) {
        lwcondvar->lw_condvar_waiter_id_list = to_wake_up->next;
        lw_waiter_remove_from_id_list(to_wake_up);
    }
    lw_mutex2b_unlock(&lwcondvar->lw_condvar_mutex);
    if (to_wake_up != NULL) {
        lw_waiter_wakeup(to_wake_up, lwcondvar);
    }
}

extern void
lw_condvar_broadcast(LW_INOUT lw_condvar_t *lwcondvar)
{
    id_t list_to_wake_up;
    lw_condvar_t old = *lwcondvar;
    if (old.lw_condvar_waiter_id_list == LW_WAITER_ID_MAX) {
        /* Nothing to signal. */
        return;
    }
    lw_mutex2b_lock(&lwcondvar->lw_condvar_mutex);
    list_to_wake_up = lwcondvar->lw_condvar_waiter_id_list;
    lwcondvar->lw_condvar_waiter_id_list = LW_WAITER_ID_MAX;
    lw_mutex2b_unlock(&lwcondvar->lw_condvar_mutex);
    lw_waiter_wake_all(lw_waiter_global_domain, list_to_wake_up, lwcondvar);

}
