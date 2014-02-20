#include "lw_cond_var.h"
#include "lw_waiter_intern.h"
#include "lw_thread.h"
#include "lw_sync_log.h"
#include "lw_debug.h"
#include "lw_atomic.h"
#include "lw_cycles.h"

#include <errno.h>

extern void
lw_condvar_wait(LW_INOUT void *_mutex,
                LW_IN lw_lock_type_t type,
                LW_INOUT lw_lock_stats_t *stats,
                LW_INOUT lw_condvar_t *lwcondvar)
{
    int ret = lw_condvar_timedwait(_mutex, type, stats, lwcondvar, NULL);
    lw_verify(ret == 0);
}

extern int
lw_condvar_timedwait(LW_INOUT void *_mutex,
                     LW_IN lw_lock_type_t type,
                     LW_INOUT lw_lock_stats_t *stats,
                     LW_INOUT lw_condvar_t *lwcondvar,
                     LW_IN struct timespec *abstime)
{
    lw_waiter_t *waiter;
    lw_waiter_t *existing_waiter;
    int wait_result = 0;

    if (stats == NULL) {
        stats = lw_lock_stats_get_global();
    }

    waiter = lw_waiter_get();
    lw_assert(waiter->lw_waiter_event.lw_te_base.lw_be_wait_src == NULL);
    lw_assert(waiter->lw_waiter_next == LW_WAITER_ID_MAX);
    lw_mutex2b_lock(&lwcondvar->cmutex, stats);
    if (lwcondvar->waiter_id_list == LW_WAITER_ID_MAX) {
        /* First waiter */
        lwcondvar->waiter_id_list = waiter->lw_waiter_id;
        waiter->lw_waiter_prev = LW_WAITER_ID_MAX;
    } else {
        for (existing_waiter = lw_waiter_from_id(lwcondvar->waiter_id_list);
             existing_waiter->lw_waiter_next != LW_WAITER_ID_MAX;
             existing_waiter = lw_waiter_from_id(existing_waiter->lw_waiter_next)) {
            /* Get to the last waiter */
        }
        existing_waiter->lw_waiter_next = waiter->lw_waiter_id;
        waiter->lw_waiter_prev = existing_waiter->lw_waiter_id;
    }
    waiter->lw_waiter_event.lw_te_base.lw_be_wait_src = lwcondvar;
    lw_mutex2b_unlock(&lwcondvar->cmutex, TRUE);
    /* Now drop the mutex and wait */
    lw_lock_common_drop_lock(_mutex, type, stats);
    wait_result = lw_waiter_timedwait(waiter, abstime);
    if (wait_result != 0) {
        lw_assert(abstime != NULL);
        lw_assert(wait_result == ETIMEDOUT);
        lw_bool_t got_signal_while_timing_out = FALSE;
        lw_mutex2b_lock(&lwcondvar->cmutex, stats);
        /* Need to extract the waiter out of the queue if it is still
         * on it.
         */
        if (waiter->lw_waiter_prev == LW_WAITER_ID_MAX &&
            waiter->lw_waiter_id != lwcondvar->waiter_id_list) {
            /* Waiter got removed from list already */
            got_signal_while_timing_out = TRUE;
            wait_result = 0;
        } else {
            if (waiter->lw_waiter_id == lwcondvar->waiter_id_list) {
                /* This is still the first waiter */
                lwcondvar->waiter_id_list = waiter->lw_waiter_next;
            }
            lw_waiter_remove_from_id_list(waiter);
            waiter->lw_waiter_event.lw_te_base.lw_be_wait_src = NULL;
        }
        lw_mutex2b_unlock(&lwcondvar->cmutex, TRUE);
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
    lw_lock_common_acquire_lock(_mutex, type, waiter, stats);

    if (stats->lw_ls_trace_history) {
        /*
         * For this to work the thread must be created using 
         * the lw_thread APIs and the lw_thread API must be
         * initialized with sync log feature on (that is
         * call lw_thread_system_init() with TRUE). Otherwise,
         * we will have a NULL returned from 
         * lw_thread_sync_log_next_line()
         */
        lw_sync_log_line_t *line = lw_thread_sync_log_next_line();
        if (line != NULL) {
            line->lw_sll_name = stats->lw_ls_name;
            line->lw_sll_lock_ptr = lwcondvar;
            line->lw_sll_start_tsc = 0;    /* The event_wait entry from underlying wait */
            line->lw_sll_end_tsc = 0;      /* has the correct values */
            line->lw_sll_primitive_type = LW_SYNC_TYPE_LWCONDVAR;
            line->lw_sll_event_id = LW_SYNC_EVENT_TYPE_COND_WAIT;
            line->lw_sll_specific_data[0] = LW_PTR_2_NUM(_mutex, lw_uint64_t);
            line->lw_sll_specific_data[1] = (lw_uint64_t)type;
            line->lw_sll_specific_data[2] = wait_result;
        }
    }

    return wait_result;

}

extern void
lw_condvar_signal(LW_INOUT lw_condvar_t *lwcondvar)
{
    lw_waiter_t *to_wake_up;
    lw_condvar_t old = *lwcondvar;
    if (old.waiter_id_list == LW_WAITER_ID_MAX) {
        /* Nothing to signal. */
        return;
    }
    lw_mutex2b_lock(&lwcondvar->cmutex, NULL);
    to_wake_up = lw_waiter_from_id(lwcondvar->waiter_id_list);
    if (to_wake_up != NULL) {
        lwcondvar->waiter_id_list = to_wake_up->lw_waiter_next;
        lw_waiter_remove_from_id_list(to_wake_up);
    }
    lw_mutex2b_unlock(&lwcondvar->cmutex, TRUE);
    if (to_wake_up != NULL) {
        lw_waiter_wakeup(to_wake_up, lwcondvar);
    }
}

extern void
lw_condvar_broadcast(LW_INOUT lw_condvar_t *lwcondvar)
{
    lw_waiter_id_t list_to_wake_up;
    lw_condvar_t old = *lwcondvar;
    if (old.waiter_id_list == LW_WAITER_ID_MAX) {
        /* Nothing to signal. */
        return;
    }
    lw_mutex2b_lock(&lwcondvar->cmutex, NULL);
    list_to_wake_up = lwcondvar->waiter_id_list;
    lwcondvar->waiter_id_list = LW_WAITER_ID_MAX;
    lw_mutex2b_unlock(&lwcondvar->cmutex, TRUE);
    lw_waiter_wake_all(lw_waiter_global_domain, list_to_wake_up, lwcondvar);

}
