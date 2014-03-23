/***
 * Developed originally at EMC Corporation, this library is released under the
 * MPL 2.0 license.  Please refer to the MPL-2.0 file in the repository for its
 * full description or to http://www.mozilla.org/MPL/2.0/ for the online version.
 *
 * Before contributing to the project one needs to sign the committer agreement
 * available in the "committerAgreement" directory.
 */

#include "lw_mutex.h"
#include "lw_waiter_intern.h"
#include "lw_debug.h"
#include "lw_atomic.h"
#include "lw_cycles.h"
#include <errno.h>

void
lw_mutex_init(LW_INOUT lw_mutex_t *lw_mutex)
{
    lw_mutex->lw_mutex_owner = LW_WAITER_ID_MAX;
    lw_mutex->lw_mutex_waitq = LW_WAITER_ID_MAX;
}

void
lw_mutex_destroy(LW_INOUT lw_mutex_t *lw_mutex)
{
    lw_verify(lw_mutex->lw_mutex_owner == LW_WAITER_ID_MAX);
    lw_verify(lw_mutex->lw_mutex_waitq == LW_WAITER_ID_MAX);
}

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
    new.lw_mutex_owner = waiter->id;
    new.lw_mutex_waitq = LW_WAITER_ID_MAX;

    lw_mutex_old_val = lw_uint32_cmpxchg(&lw_mutex->lw_mutex_val,
                                          old.lw_mutex_val,
                                          new.lw_mutex_val);

    got_lock = (lw_mutex_old_val == old.lw_mutex_val);
    lw_assert(!got_lock || lw_mutex->lw_mutex_owner == waiter->id);
    return (got_lock ? 0 : EBUSY);
}

/* Lock an lw_mutex. */
void
lw_mutex_lock(LW_INOUT lw_mutex_t *lw_mutex)
{
    lw_mutex_t old;
    lw_mutex_t new;
    lw_waiter_t *waiter;

    waiter = lw_waiter_get();
    lw_assert(waiter->event.base.wait_src == NULL);
    lw_assert(waiter->next == LW_WAITER_ID_MAX);
    lw_assert(waiter->prev == LW_WAITER_ID_MAX);
    old = *lw_mutex;
    do {
        new = old;
        if (old.lw_mutex_owner == LW_WAITER_ID_MAX) {
            /* Currently unlocked. */
            lw_assert(old.lw_mutex_waitq == LW_WAITER_ID_MAX);
            new.lw_mutex_owner = waiter->id;
            waiter->next = LW_WAITER_ID_MAX;
            waiter->event.base.wait_src = NULL;
        } else {
            lw_verify(old.lw_mutex_owner != waiter->id);
            waiter->next = old.lw_mutex_waitq;
            waiter->event.base.wait_src = lw_mutex;
            new.lw_mutex_waitq = waiter->id;
        }
    } while (!lw_uint32_swap(&lw_mutex->lw_mutex_val,
                             &old.lw_mutex_val,
                             new.lw_mutex_val));

    if (new.lw_mutex_owner != waiter->id) {
        /* Did not get lock. Must wait */
        lw_assert(new.lw_mutex_waitq == waiter->id);
        lw_waiter_wait(waiter);
        /* The thread waking this up will also transfer the lock to it.
         * No need to retry getting the lock.
         */
        lw_verify(lw_mutex->lw_mutex_owner == waiter->id);
    }
}

lw_bool_t
lw_mutex_lock_if_not_held(LW_INOUT lw_mutex_t *lw_mutex)
{
    lw_mutex_t old;
    lw_bool_t retval = FALSE;
    lw_waiter_t *waiter = lw_waiter_get();
    old.lw_mutex_val = lw_mutex->lw_mutex_val;
    if (old.lw_mutex_owner == LW_WAITER_ID_MAX ||
        old.lw_mutex_owner != waiter->id) {
        /* Lock not held by caller. */
        retval = TRUE;
    } else {
        /* Caller has lock already */
        return FALSE;
    }
    lw_mutex_lock(lw_mutex);
    return retval;
}

void
lw_mutex_unlock_if_held(LW_INOUT lw_mutex_t *lw_mutex)
{
    lw_mutex_t old;
    lw_waiter_t *waiter = lw_waiter_get();
    old.lw_mutex_val = lw_mutex->lw_mutex_val;

    if (old.lw_mutex_owner == waiter->id) {
        /* Lock held by caller. */
        lw_assert(old.lw_mutex_owner != LW_WAITER_ID_MAX);
        lw_mutex_unlock(lw_mutex);
    }
}

static void
lw_mutex_setup_prev_id_pointers(LW_INOUT lw_mutex_t *lw_mutex,
                                LW_IN lw_uint32_t id,
                                LW_INOUT lw_waiter_t **lastwaiterp)
{
    lw_waiter_t *waiter;
    lw_waiter_t *next_waiter;
    id_t waiter_id = id;

    LW_UNUSED_PARAMETER(lw_mutex); /* Only used to check wait_src */
    lw_assert(lastwaiterp != NULL);
    if (waiter_id == LW_WAITER_ID_MAX) {
        *lastwaiterp = NULL;
        return;
    }

    lw_assert(waiter_id < LW_WAITER_ID_MAX);
    waiter = lw_waiter_from_id(waiter_id);
    lw_assert(waiter->event.base.wait_src == lw_mutex);
    while (waiter->next != LW_WAITER_ID_MAX) {
        next_waiter = lw_waiter_from_id(waiter->next);
        lw_assert(next_waiter->prev == LW_WAITER_ID_MAX ||
                  next_waiter->prev == waiter->id);
        next_waiter->prev = waiter->id;
        waiter = next_waiter;
        lw_assert(waiter->event.base.wait_src == lw_mutex);
    }
    *lastwaiterp = waiter;
    return;
}

void
lw_mutex_unlock(LW_INOUT lw_mutex_t *lw_mutex)
{
    lw_mutex_t old;
    lw_mutex_t new;
    lw_waiter_t *waiter;
    lw_waiter_t *waiter_to_wake_up = NULL;

    old.lw_mutex_val = lw_mutex->lw_mutex_val;
    waiter = lw_waiter_get();
    lw_verify(old.lw_mutex_owner == waiter->id); /* Enforce posix semantics */
    do {
        new = old;
        lw_mutex_setup_prev_id_pointers(lw_mutex,
                                        old.lw_mutex_waitq,
                                        &waiter_to_wake_up);
        if (waiter_to_wake_up != NULL) {
            new.lw_mutex_owner = waiter_to_wake_up->id;
            if (new.lw_mutex_waitq == waiter_to_wake_up->id) {
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
        lw_assert(new.lw_mutex_owner == waiter_to_wake_up->id);
        lw_assert(new.lw_mutex_waitq != waiter_to_wake_up->id);
        lw_waiter_wakeup(waiter_to_wake_up, lw_mutex);
    } else {
        lw_assert(new.lw_mutex_waitq == LW_WAITER_ID_MAX);
        lw_assert(new.lw_mutex_owner == LW_WAITER_ID_MAX);
    }
}



#ifdef LW_DEBUG
void
lw_mutex_assert_locked(lw_mutex_t *lw_mutex)
{
    lw_waiter_t *waiter;
    waiter = lw_waiter_get();
    lw_assert(lw_mutex->lw_mutex_owner == waiter->id);
}

void
lw_mutex_assert_not_locked(lw_mutex_t *lw_mutex)
{
    lw_waiter_t *waiter;
    waiter = lw_waiter_get();
    lw_assert(lw_mutex->lw_mutex_owner != waiter->id);
}
#endif


