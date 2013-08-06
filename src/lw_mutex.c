#include "lw_mutex.h"
#include "lw_waiter_intern.h"
lw_int32_t
lw_mutex_trylock(LW_INOUT lw_mutex_t *lw_mutex)
{
    lw_mutex_t old;
    lw_mutex_t new;
    lw_waiter_t *waiter;
    lw_bool_t got_lock;
    lw_uint32_t lw_mutex_old_val;

    old.lw_mutex_val = lw_mutex->lw_mutex_val;
    if (old.lw_mutex_owner != LW_WAITER_ID_MAX) {
        /* Currently held lock. */
        return EBUSY;
    }   
    lw_assert(old.lw_mutex_waitq == LW_WAITER_ID_MAX);
    waiter = lw_waiter_get();
    new.lw_mutex_owner = waiter->lw_waiter_id;
    new.lw_mutex_waitq = LW_WAITER_ID_MAX;

    lw_mutex_old_val = lw_uint32_cmpxchg(&lw_mutex->lw_mutex_val, 
                                          old.lw_mutex_val, 
                                          new.lw_mutex_val)

    got_lock = (lw_mutex_old_val == old.lw_mutex_val);
    lw_assert(!got_lock || lw_mutex->lw_mutex_owner == waiter->lw_waiter_id);
    return (got_lock ? 0 : EBUSY);
}   

/* Lock an lw_mutex. */
void
lw_mutex_lock(LW_INOUT lw_mutex_t *lw_mutex,
              LW_INOUT lw_lock_stats_t *lw_lock_stats)
{
    lw_mutex_t old;
    lw_mutex_t new;
    lw_waiter_t *waiter;
    lw_uint64_t tsc_beg;
    lw_uint64_t tsc_end;

    if (lw_lock_stats == NULL) {
        lw_lock_stats = lw_lock_stats_get_global();
    }
    
    tsc_beg = lw_rdtsc(); 
    waiter = lw_waiter_get();
    lw_assert(waiter->lw_waiter_event.lw_te_base.lw_be_wait_src == NULL);
    lw_assert(waiter->lw_waiter_next == LW_WAITER_ID_MAX);
    lw_assert(waiter->lw_waiter_prev == LW_WAITER_ID_MAX);
    old = *lw_mutex;
    do {
        new = old;
        if (old.lw_mutex_owner == LW_WAITER_ID_MAX) {
            /* Currently unlocked. */
            lw_assert(old.lw_mutex_waitq == LW_WAITER_ID_MAX);
            new.lw_mutex_owner = waiter->lw_waiter_id;
            waiter->lw_waiter_next = LW_WAITER_ID_MAX;
            waiter->lw_waiter_event.lw_te_base.lw_be_wait_src = NULL;
        } else {
            lw_verify(old.lw_mutex_owner != waiter->lw_waiter_id);
            waiter->lw_waiter_next = old.lw_mutex_waitq;
            waiter->lw_waiter_event.lw_te_base.lw_be_wait_src = lw_mutex;
            new.lw_mutex_waitq = waiter->lw_waiter_id;
        }
    } while (!lw_uint32_swap(&lw_mutex->lw_mutex_val, 
                             &old.lw_mutex_val, 
                             new.lw_mutex_val));

    if (new.lw_mutex_owner != waiter->lw_waiter_id) {
        /* Did not get lock. Must wait */
        lw_assert(new.lw_mutex_waitq == waiter->lw_waiter_id);
        lw_waiter_wait(waiter);
        /* The thread waking this up will also transfer the lock to it.
         * No need to retry getting the lock.
         */
        lw_verify(lw_mutex->lw_mutex_owner == waiter->lw_waiter_id);
        /* increment contention stats */
        tsc_end = lw_rdtsc();
        lw_atomic64_add(&lw_lock_stats->lw_ls_lock_contention_cyc, 
                        LW_TSC_DIFF(tsc_end, tsc_beg));
        lw_atomic32_inc(&lw_lock_stats->lw_ls_lock_contentions);
    } else {
        tsc_end = tsc_beg;
    }

    if (lw_lock_stats->lw_ls_trace_history) {
        /*
         * For this to work the caller has to have registered
         * the synchronization log mechanism by calling 
         * lw_sync_log_register once for this thread.
         */
        lw_sync_log_line_t *line = lw_sync_log_next_line();
        if (line != NULL) {
            line->lw_sll_name = lw_lock_stats->lw_ls_name;
            line->lw_sll_lock_ptr = lw_mutex;
            line->lw_sll_start_tsc = tsc_beg;
            line->lw_sll_end_tsc = tsc_end;
            line->lw_sll_primitive_type = LW_SYNC_TYPE_LWMUTEX;
            line->lw_sll_event_id = LW_SYNC_EVENT_TYPE_MUTEX_LOCK;
        }
    }
}

lw_bool_t
lw_mutex_lock_if_not_held(LW_INOUT lw_mutex_t *lw_mutex,
                          LW_INOUT lw_lock_stats_t *lw_lock_stats)
{
    lw_mutex_t old;
    lw_bool_t retval = FALSE;
    lw_waiter_t *waiter = lw_waiter_get(); 
    old.lw_mutex_val = lw_mutex->lw_mutex_val;
    if (old.lw_mutex_owner == LW_WAITER_ID_MAX || 
        old.lw_mutex_owner != waiter->lw_waiter_id) {
        /* Lock not held by caller. */
        retval = TRUE;
    } else {
        /* Caller has lock already */
        return FALSE;
    }
    lw_mutex_lock(lw_mutex, lw_lock_stats);
    return retval;
}

void
lw_mutex_unlock_if_held(LW_INOUT lw_mutex_t *lw_mutex)
{
    lw_mutex_t old;
    lw_waiter_t *waiter = lw_waiter_get(); 
    old.lw_mutex_val = lw_mutex->lw_mutex_val;

    if (old.lw_mutex_owner == waiter->lw_waiter_id) {
        /* Lock held by caller. */
        lw_assert(old.lw_mutex_owner != LW_WAITER_ID_MAX);
        lw_mutex_unlock(lw_mutex, TRUE);
    }
}

static void
lw_mutex_setup_prev_id_pointers(LW_INOUT lw_mutex_t *lw_mutex, 
                                LW_IN lw_uint32_t id, 
                                LW_INOUT lw_waiter_t **lastwaiterp)
{
    lw_waiter_t *waiter;
    lw_waiter_t *next_waiter;
    lw_waiter_id_t waiter_id = id;

    LW_UNUSED_PARAMETER(lw_mutex); /* Only used to check wait_src */
    lw_assert(lastwaiterp != NULL);
    if (waiter_id == LW_WAITER_ID_MAX) {
        *lastwaiterp = NULL;
        return;
    }

    lw_assert(_id < LW_WAITER_ID_MAX);
    waiter = lw_waiter_from_id(waiter_id);
    lw_assert(waiter->lw_waiter_event.lw_te_base.lw_be_wait_src == lw_mutex);
    while (waiter->lw_waiter_next != LW_WAITER_ID_MAX) {
        next_waiter = lw_waiter_from_id(waiter->lw_waiter_next);
        lw_assert(next_waiter->lw_waiter_prev == LW_WAITER_ID_MAX ||
                  next_waiter->lw_waiter_prev == waiter->lw_waiter_id);
        next_waiter->lw_waiter_prev = waiter->lw_waiter_id;
        waiter = next_waiter;
        lw_assert(waiter->lw_waiter_event.lw_te_base.lw_be_wait_src == lw_mutex);
    }
    *lastwaiterp = waiter;
    return;
}

void
lw_mutex_unlock(LW_INOUT lw_mutex_t *lw_mutex, 
                LW_IN lw_bool_t trace)
{
    lw_mutex_t old;
    lw_mutex_t new;
    lw_waiter_t *waiter;
    lw_waiter_t *waiter_to_wake_up = NULL;

    old.lw_mutex_val = lw_mutex->lw_mutex_val;
    waiter = lw_waiter_get();
    lw_verify(old.lw_mutex_owner == waiter->lw_waiter_id); /* Enforce posix semantics */
    do {
        new = old;
        lw_mutex_setup_prev_id_pointers(lw_mutex, 
                                        old.lw_mutex_waitq, 
                                        &waiter_to_wake_up);
        if (waiter_to_wake_up != NULL) {
            new.lw_mutex_owner = waiter_to_wake_up->lw_waiter_id;
            if (new.lw_mutex_waitq == waiter_to_wake_up->lw_waiter_id) {
                /* This is the only waiter */
                new.lw_mutex_waitq = LW_WAITER_ID_MAX;
            }
        } else {
            lw_assert(old.lw_mutex_waitq == LW_WAITER_ID_MAX);
            new.lw_mutex_owner = LW_WAITER_ID_MAX;
        }
    } while (!lw_uint32_swap(&lw_mutex->lw_mutex_val, 
                             &old.lw_mutex_val, 
                             new.lw_mutex_val));

    if (waiter_to_wake_up != NULL) {
        lw_waiter_remove_from_id_list(waiter_to_wake_up);
        lw_assert(new.lw_mutex_owner == waiter_to_wake_up->lw_waiter_id);
        lw_assert(new.lw_mutex_waitq != waiter_to_wake_up->lw_waiter_id);
        lw_waiter_wakeup(waiter_to_wake_up, lw_mutex);
    } else {
        lw_assert(new.lw_mutex_waitq == LW_WAITER_ID_MAX);
        lw_assert(new.lw_mutex_owner == LW_WAITER_ID_MAX);
    }

    if (trace) {
        /*
         * For this to work the caller has to have registered
         * the synchronization log mechanism by calling 
         * lw_sync_log_register once for this thread.
         */
        lw_sync_log_line_t *line = lw_sync_log_next_line();
        if (line != NULL) {
            line->lw_sll_name = NULL;
            line->lw_sll_lock_ptr = lw_mutex;
            line->lw_sll_start_tsc = lw_rdtsc();
            line->lw_sll_end_tsc = line->lw_sll_start_tsc;
            line->lw_sll_primitive_type = LW_SYNC_TYPE_LWMUTEX;
            line->lw_sll_event_id = LW_SYNC_EVENT_TYPE_MUTEX_UNLOCK;
        }
    }
}
