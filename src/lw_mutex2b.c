/***
 * Developed originally at EMC Corporation, this library is released under the
 * MPL 2.0 license.  Please refer to the MPL-2.0 file in the repository for its
 * full description or to http://www.mozilla.org/MPL/2.0/ for the online version.
 *
 * Before contributing to the project one needs to sign the committer agreement
 * available in the "committerAgreement" directory.
 */

#include "lw_mutex2b.h"
#include "lw_mutex.h"
#include "lw_waiter_intern.h"
#include "lw_debug.h"
#include "lw_atomic.h"
#include "lw_cycles.h"
#include <errno.h>

void
lw_mutex2b_init(LW_INOUT lw_mutex2b_t *lw_mutex2b)
{
    *lw_mutex2b = LW_MUTEX2B_INITIALIZER;
}

void
lw_mutex2b_destroy(LW_INOUT lw_mutex2b_t *lw_mutex2b)
{
    lw_verify(*lw_mutex2b == LW_WAITER_ID_MAX);
}

lw_int32_t
lw_mutex2b_trylock(LW_INOUT lw_mutex2b_t *lw_mutex2b)
{
    lw_waiter_t *waiter;
    lw_bool_t got_lock;
    lw_uint16_t lw_mutex2b_old_val;
    if (*lw_mutex2b != LW_WAITER_ID_MAX) {
        /* Currently held lock. */
        return EBUSY;
    }
    waiter = lw_waiter_get();

    lw_mutex2b_old_val = lw_uint16_cmpxchg(lw_mutex2b,
                                           LW_WAITER_ID_MAX,
                                           waiter->id);

    got_lock = (lw_mutex2b_old_val == LW_WAITER_ID_MAX);

    return (got_lock ? 0 : EBUSY);
}

static lw_waiter_t *
lw_mutex2b_find_oldest_waiter(lw_uint32_t _waitq, lw_uint32_t _owner)
{
    lw_waiter_t *waiter = NULL;
    id_t waitq = _waitq;
    id_t owner = _owner;
    lw_assert(_waitq < LW_WAITER_ID_MAX);
    lw_assert(_owner < LW_WAITER_ID_MAX);
    lw_assert(waitq != owner);

    for (waiter = lw_waiter_from_id(waitq);
         waiter != NULL && waiter->next != owner;
         waiter = lw_waiter_from_id(waiter->next)) {
        /* Do nothing */
    }
    return waiter;
}

void
lw_mutex2b_lock_with_waiter(LW_INOUT lw_mutex2b_t *lw_mutex2b, lw_waiter_t *waiter)
{
    lw_mutex2b_t old;

#ifdef LW_DEBUG
    { /* Deadlock detection. */
         old = *lw_mutex2b;

         /* Owner cannot be the back of the waiting queue */
         lw_verify(old != waiter->id);

         lw_waiter_t *const oldest_waiter =
             (old == LW_WAITER_ID_MAX) ?
             NULL :
             lw_mutex2b_find_oldest_waiter(old, waiter->id);
        /* Make the assert below a verify to enable deadlock detection.  Note
         * that this check is very tricky. To ensure we aren't going to
         * deadlock, the walk down the queue to find the oldest_waiter is racy.
         * As such it could actually find a waiter that does indeed point to the
         * this thread's waiter: That can happen because the waiter got wokeup
         * and got requeued to some other lw_mutex2b that this thread happens to
         * own. Hence the check has to be for multiple conditions: We don't find
         * a waiter or if we do, that waiter is no longer waiting on this
         * lw_mutex2b or it isn't really pointing to this waiter anymore. This
         * will get especially tricky if we also start doing async locking for
         * lw_mutex2b. All in all, it is better to just give up on deadlock
         * detection for extra small-size lw_mutex2b. Use the 4 byte variant
         * (which explicitly tracks owner) instead.
         */
        lw_verify(oldest_waiter == NULL ||
                  oldest_waiter->event.wait_src != lw_mutex2b ||
                  oldest_waiter->next != waiter->id);
    }
#endif

    lw_waiter_set_src(waiter, lw_mutex2b);
    do {
        old = *lw_mutex2b;
        lw_assert(old != waiter->id);
        waiter->next = old;
    } while (lw_uint16_cmpxchg(lw_mutex2b, old, waiter->id) != old);

    if (old != LW_WAITER_ID_MAX) {
        /* Did not get lock. Must wait */
        lw_waiter_wait(waiter);
        /* The thread waking this up will also transfer the lock to it.
         * No need to retry getting the lock.
         */
        lw_mutex2b_assert_locked(lw_mutex2b);
    }
    /* Clear the wait_src pointer set earlier */
    lw_waiter_clear_src(waiter);
}

void
lw_mutex2b_unlock_with_waiter(LW_INOUT lw_mutex2b_t *lw_mutex2b, lw_waiter_t *waiter)
{
    lw_mutex2b_t old;
    lw_mutex2b_t new;
    lw_waiter_t *waiter_to_wake_up = NULL;
    id_t owner_id;

    owner_id = waiter->id;
    do {
        old = *lw_mutex2b;
        new = old;
        if (old == owner_id) {
            /* No waiters. */
            new = LW_WAITER_ID_MAX;
        } else if (old != LW_WAITER_ID_MAX && waiter_to_wake_up == NULL) {
            /* We have waiters. If we ever support async locking in lw_mutex2b,
             * the if check will need to be tempered (by looking at wait_src
             * of waiter) as well as the owner could also be waiting. The
             * better thing at that point would be to use 4 byte lw_mutex2b to
             * explicitly separate waiters from owner.
             */
            waiter_to_wake_up = lw_mutex2b_find_oldest_waiter(old, owner_id);
        }
        lw_assert(old == owner_id || /* No waiters OR */
                  /* There is some waiter and */
                  ((old != LW_WAITER_ID_MAX) &&
                   /* we have waiter to wake up and */
                   (waiter_to_wake_up != NULL) &&
                   /* that waiter points to owner (fairness) */
                   (waiter_to_wake_up->next == owner_id)));
    } while (lw_uint16_cmpxchg(lw_mutex2b, old, new) != old);

    if (waiter_to_wake_up != NULL) {
        lw_assert(new == waiter_to_wake_up->id ||
                  new != LW_WAITER_ID_MAX);
        waiter_to_wake_up->next = LW_WAITER_ID_MAX;
        lw_waiter_wakeup(waiter_to_wake_up, lw_mutex2b);
    } else {
        /* Verify that we really had no waiters. */
        lw_verify(old == owner_id && new == LW_WAITER_ID_MAX);
    }
}

#ifdef LW_DEBUG
void
lw_mutex2b_assert_locked(LW_INOUT lw_mutex2b_t *lw_mutex2b)
{
    lw_waiter_t *waiter;
    lw_waiter_t *oldest_waiter;
    lw_mutex2b_t old;
    waiter = lw_waiter_get();
    old = *lw_mutex2b;
    if (old == waiter->id) {
        return;
    }
    oldest_waiter = lw_mutex2b_find_oldest_waiter(old, waiter->id);
    lw_assert((oldest_waiter->next == waiter->id) &&
              (oldest_waiter->event.wait_src == lw_mutex2b));
}
void
lw_mutex2b_assert_not_locked(LW_INOUT lw_mutex2b_t *lw_mutex2b)
{
    lw_waiter_t *waiter;
    lw_waiter_t *oldest_waiter;
    lw_mutex2b_t old;
    waiter = lw_waiter_get();
    old = *lw_mutex2b;
    lw_assert(old != waiter->id);
    oldest_waiter = lw_mutex2b_find_oldest_waiter(old,
                                                  waiter->id);
    /* The walk to get the oldest_waiter is racy since the calling
     * thread is not expected to own the lw_mutex2b. So it is possible
     * that the list member move from one lw_mutex2b to another wait list
     * and we indeed end up finding an "oldest_waiter". However, it should
     * not be waiting on this lw_mutex2b in that case.
     */
    lw_assert((oldest_waiter == NULL) ||
              (oldest_waiter->event.wait_src != lw_mutex2b) ||
              (oldest_waiter->next != waiter->id));
}
#endif

