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
                                           waiter->lw_waiter_id);

    got_lock = (lw_mutex2b_old_val == LW_WAITER_ID_MAX);

    return (got_lock ? 0 : EBUSY);
}

static lw_waiter_t *
lw_mutex2b_find_oldest_waiter(lw_uint32_t _waitq, lw_uint32_t _owner)
{
    lw_waiter_t *waiter = NULL;
    lw_waiter_id_t waitq = _waitq;
    lw_waiter_id_t owner = _owner;
    lw_assert(_waitq < LW_WAITER_ID_MAX);
    lw_assert(_owner < LW_WAITER_ID_MAX);
    lw_assert(waitq != owner);

    for (waiter = lw_waiter_from_id(waitq);
         waiter != NULL && waiter->lw_waiter_next != owner;
         waiter = lw_waiter_from_id(waiter->lw_waiter_next)) {
        /* Do nothing */
    }
    return waiter;
}

void
lw_mutex2b_lock(LW_INOUT lw_mutex2b_t *lw_mutex2b)
{
    lw_mutex2b_t old;
    lw_waiter_t *waiter;

    waiter = lw_waiter_get();
#ifdef LW_DEBUG
    { /* Deadlock detection. */
         old = *lw_mutex2b;

         /* Owner cannot be the back of the waiting queue */
         lw_verify(old != waiter->lw_waiter_id);

         lw_waiter_t *const oldest_waiter =
             (old == LW_WAITER_ID_MAX) ?
             NULL :
             lw_mutex2b_find_oldest_waiter(old, waiter->lw_waiter_id);
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
                  oldest_waiter->lw_waiter_event.lw_te_base.lw_be_wait_src != lw_mutex2b ||
                  oldest_waiter->lw_waiter_next != waiter->lw_waiter_id);
    }
#endif

    do {
        old = *lw_mutex2b;
        lw_assert(old != waiter->lw_waiter_id);
        waiter->lw_waiter_next = old;
        waiter->lw_waiter_event.lw_te_base.lw_be_wait_src = lw_mutex2b;
    } while (lw_uint16_cmpxchg(lw_mutex2b, old, waiter->lw_waiter_id) != old);

    if (old != LW_WAITER_ID_MAX) {
        /* Did not get lock. Must wait */
        lw_waiter_wait(waiter);
        /* The thread waking this up will also transfer the lock to it.
         * No need to retry getting the lock.
         */
        lw_mutex2b_assert_locked(lw_mutex2b);
    } else {
        /* Clear the wait_src pointer set earlier */
        waiter->lw_waiter_event.lw_te_base.lw_be_wait_src = NULL;
    }
}

void
lw_mutex2b_unlock(LW_INOUT lw_mutex2b_t *lw_mutex2b)
{
    lw_mutex2b_t old;
    lw_mutex2b_t new;
    lw_waiter_t *waiter;
    lw_waiter_t *waiter_to_wake_up = NULL;
    lw_waiter_id_t owner_id;

    waiter = lw_waiter_get();
    owner_id = waiter->lw_waiter_id;
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
                   (waiter_to_wake_up->lw_waiter_next == owner_id)));
    } while (lw_uint16_cmpxchg(lw_mutex2b, old, new) != old);

    if (waiter_to_wake_up != NULL) {
        lw_assert(new == waiter_to_wake_up->lw_waiter_id ||
                  new != LW_WAITER_ID_MAX);
        waiter_to_wake_up->lw_waiter_next = LW_WAITER_ID_MAX;
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
    if (old == waiter->lw_waiter_id) {
        return;
    }
    oldest_waiter = lw_mutex2b_find_oldest_waiter(old,
                                                  waiter->lw_waiter_id);
    lw_assert((oldest_waiter->lw_waiter_next == waiter->lw_waiter_id) &&
              (oldest_waiter->lw_waiter_event.lw_te_base.lw_be_wait_src ==
              lw_mutex2b));
}
void
lw_mutex2b_assert_not_locked(LW_INOUT lw_mutex2b_t *lw_mutex2b)
{
    lw_waiter_t *waiter;
    lw_waiter_t *oldest_waiter;
    lw_mutex2b_t old;
    waiter = lw_waiter_get();
    old = *lw_mutex2b;
    lw_assert(old != waiter->lw_waiter_id);
    oldest_waiter = lw_mutex2b_find_oldest_waiter(old,
                                                  waiter->lw_waiter_id);
    /* The walk to get the oldest_waiter is racy since the calling
     * thread is not expected to own the lw_mutex2b. So it is possible
     * that the list member move from one lw_mutex2b to another wait list
     * and we indeed end up finding an "oldest_waiter". However, it should
     * not be waiting on this lw_mutex2b in that case.
     */
    lw_assert((oldest_waiter == NULL) ||
              (oldest_waiter->lw_waiter_event.lw_te_base.lw_be_wait_src !=
              lw_mutex2b) ||
              (oldest_waiter->lw_waiter_next != waiter->lw_waiter_id));
}
#endif

