/***
 * Developed originally at EMC Corporation, this library is released under the
 * MPL 2.0 license.  Please refer to the MPL-2.0 file in the repository for its
 * full description or to http://www.mozilla.org/MPL/2.0/ for the online version.
 *
 * Before contributing to the project one needs to sign the committer agreement
 * available in the "committerAgreement" directory.
 */

#include "lw_rwlock.h"
#include "lw_debug.h"
#include "lw_atomic.h"
#include "lw_cycles.h"
#include "lw_util.h"
#include <errno.h>

void
lw_rwlock_init(LW_INOUT lw_rwlock_t *rwlock,
               LW_IN lw_rwlock_flags_t flags)
{
    rwlock->lw_rwlock_unfair = (flags & LW_RWLOCK_UNFAIR) ? 1 : 0;
    rwlock->lw_rwlock_wlocked = 0;
    rwlock->lw_rwlock_readers = 0;
    rwlock->lw_rwlock_waitq = LW_WAITER_ID_MAX;
}

void
lw_rwlock_destroy(LW_INOUT lw_rwlock_t *rwlock)
{
    lw_verify(!rwlock->lw_rwlock_locked);
    lw_verify(rwlock->lw_rwlock_waitq == LW_WAITER_ID_MAX);
}

static int
lw_rwlock_lock_contention(LW_INOUT lw_rwlock_t *rwlock,
                          LW_IN lw_rwlock_attempt_t type,
                          LW_INOUT lw_waiter_t *waiter)
{
    lw_rwlock_t old;
    lw_rwlock_t new;
    lw_bool_t exclusive = ((type & LW_RWLOCK_EXCLUSIVE) == LW_RWLOCK_EXCLUSIVE);
    lw_bool_t wait_inline = ((type & LW_RWLOCK_WAIT_DEFERRED) != LW_RWLOCK_WAIT_DEFERRED);
    lw_uint64_t const tag_val = (type & (LW_RWLOCK_EXCLUSIVE |
                                         LW_RWLOCK_SHARED |
                                         LW_RWLOCK_UPGRADE));
    lw_assert(!(type & LW_RWLOCK_NOWAIT));

    int result = 0;

    lw_assert(waiter == NULL ||
              (waiter->lw_waiter_next == LW_WAITER_ID_MAX &&
               waiter->lw_waiter_event.lw_te_base.lw_be_wait_src == NULL));

    old = *rwlock;
    do {
        new = old;

        if (waiter != NULL) {
            /* going around this loop more than once, needs to reset waiter->lw_waiter_next */
            waiter->lw_waiter_next = LW_WAITER_ID_MAX;
        }

        if (!exclusive &&
            !new.lw_rwlock_wlocked &&
            (new.lw_rwlock_waitq == LW_WAITER_ID_MAX || new.lw_rwlock_unfair)) {
            /* A shared lock is granted if not already exclusively locked
             * and the lock is either unfair or there are no other waiters.
             */
            new.lw_rwlock_readers++;
            lw_assert(new.lw_rwlock_readers > 0); /* in case of overflow */

        } else if (exclusive &&
                   !new.lw_rwlock_locked &&
                   new.lw_rwlock_waitq == LW_WAITER_ID_MAX) {
            /*
             * An exclusive lock is granted if not already locked (shared or exclusive)
             * and there are no other waiters.
             */
            new.lw_rwlock_wlocked = 1;

        } else {
            /* Can't acquire lock, go to sleep later */
            if (waiter == NULL) {
                waiter = lw_waiter_get();
            }
            lw_assert(waiter != NULL);
            waiter->lw_waiter_event.lw_te_base.lw_be_tag = tag_val;
            lw_assert(waiter->lw_waiter_next == LW_WAITER_ID_MAX);
            lw_assert(waiter->lw_waiter_id != new.lw_rwlock_waitq);
            waiter->lw_waiter_next = new.lw_rwlock_waitq;
            new.lw_rwlock_waitq = waiter->lw_waiter_id;
            lw_assert(new.lw_rwlock_waitq != LW_WAITER_ID_MAX);
        }

    } while (!lw_uint32_swap(&rwlock->lw_rwlock_val, &old.lw_rwlock_val, new.lw_rwlock_val));

    /* block wait if we didn't get the lock */
    if (waiter != NULL && new.lw_rwlock_waitq == waiter->lw_waiter_id) {
        lw_assert(new.lw_rwlock_waitq == waiter->lw_waiter_id);
        if (wait_inline) {
            lw_assert(waiter->lw_waiter_event.lw_te_base.lw_be_wait_src == NULL);
            waiter->lw_waiter_event.lw_te_base.lw_be_wait_src = rwlock;
            lw_waiter_wait(waiter);
            /* on contention, the lock is "transferred" to the blocked threads in FIFO order */
            lw_assert(waiter->lw_waiter_event.lw_te_base.lw_be_tag == tag_val);
            lw_assert((exclusive &&
                       rwlock->lw_rwlock_wlocked &&
                       rwlock->lw_rwlock_readers == 0) ||
                      (!exclusive &&
                       !rwlock->lw_rwlock_wlocked &&
                       rwlock->lw_rwlock_readers > 0));
        }

        result = (wait_inline ? 0 : EWOULDBLOCK);
    } else {
        result = 0;
    }

    return result;
}

int
lw_rwlock_lock(LW_INOUT lw_rwlock_t *rwlock,
               LW_IN lw_rwlock_attempt_t type,
               LW_INOUT lw_waiter_t *waiter)
{
    lw_rwlock_t old;
    lw_rwlock_t new;
    lw_bool_t exclusive = ((type & LW_RWLOCK_EXCLUSIVE) == LW_RWLOCK_EXCLUSIVE);
    lw_bool_t non_blocking = ((type & LW_RWLOCK_NOWAIT) == LW_RWLOCK_NOWAIT);

    /* No need for waiter on try lock attempts */
    lw_assert(!(non_blocking && waiter != NULL));
    /* Cannot be an async wait if the attempt is non-waiting (trylock) */
    lw_assert(!(non_blocking && (type & LW_RWLOCK_WAIT_DEFERRED) == LW_RWLOCK_WAIT_DEFERRED));

    old.lw_rwlock_val = rwlock->lw_rwlock_val;
    do {
        new = old;

        if (!exclusive &&
            !new.lw_rwlock_wlocked &&
            (new.lw_rwlock_waitq == LW_WAITER_ID_MAX || new.lw_rwlock_unfair)) {
            /* A shared lock is granted if not already exclusively locked
             * and the lock is either unfair or there are no other waiters.
             */
            new.lw_rwlock_readers++;
            lw_assert(new.lw_rwlock_readers > 0); /* in case of overflow */

        } else if (exclusive &&
                   !new.lw_rwlock_locked &&
                   new.lw_rwlock_waitq == LW_WAITER_ID_MAX) {
            /* A exclusive lock is granted if not already locked (shared or exclusive)
             * and there are no other waiters.
             */
            new.lw_rwlock_wlocked = 1;

        } else if (non_blocking) {
            /* Can't acquire lock, and caller asked for non-blocking */
            return EBUSY;

        } else {
            /* Can't acquire lock, and caller wants to block wait. */
            return lw_rwlock_lock_contention(rwlock, type, waiter);
        }

    } while (!lw_uint32_swap(&rwlock->lw_rwlock_val, &old.lw_rwlock_val, new.lw_rwlock_val));

    return 0;
}

void
lw_rwlock_contention_wait(LW_INOUT lw_rwlock_t *rwlock,
                          LW_IN lw_rwlock_attempt_t type,
                          LW_INOUT lw_waiter_t *waiter)
{
#ifdef LW_DEBUG
    lw_bool_t const exclusive = ((type & LW_RWLOCK_EXCLUSIVE) == LW_RWLOCK_EXCLUSIVE);
    lw_uint64_t const tag_val = (type & (LW_RWLOCK_EXCLUSIVE | LW_RWLOCK_SHARED | LW_RWLOCK_UPGRADE));
#else
    LW_UNUSED_PARAMETER(type);
#endif

    lw_assert(waiter->lw_waiter_event.lw_te_base.lw_be_wait_src == NULL);
    waiter->lw_waiter_event.lw_te_base.lw_be_wait_src = rwlock;

    lw_assert(waiter != NULL);

    lw_waiter_wait(waiter);
    /* on contention, the lock is "transferred" to the blocked threads in FIFO order */
#ifdef LW_DEBUG
    lw_assert(waiter->lw_waiter_event.lw_te_base.lw_be_tag == tag_val);
    lw_assert((exclusive &&
               rwlock->lw_rwlock_wlocked &&
               rwlock->lw_rwlock_readers == 0) ||
              (!exclusive &&
               !rwlock->lw_rwlock_wlocked &&
               rwlock->lw_rwlock_readers > 0));
#endif
}

#ifdef LW_DEBUG
/*
 * This function verifies that wakeup list and waiter list do not have
 * overlapping elements.
 */
static void
lw_rwlock_check_waitq_membership(lw_uint32_t waitq, lw_uint32_t wakeup_list)
{
    lw_waiter_t *waiter;
    lw_waiter_t *wakeup;
    wakeup = lw_waiter_from_id(wakeup_list);
    while (wakeup != NULL) {
        waiter = lw_waiter_from_id(waitq);
        while (waiter != NULL) {
            lw_verify(waiter != wakeup);
            waiter = lw_waiter_from_id(waiter->lw_waiter_next);
        }
        wakeup = lw_waiter_from_id(wakeup->lw_waiter_next);
    }
}
#endif

/*
 * Internal function to wake up waiters when unlocking a rwlock. This is called
 * for fair locks. For unfair locks, this is called when transferring the lock to
 * a writer.
 */
static void
lw_rwlock_unlock_fair_contention(LW_INOUT lw_rwlock_t *rwlock,
                                 LW_IN lw_bool_t exclusive)
{
    lw_rwlock_t old;
    lw_rwlock_t new;
    lw_waiter_t *waiter = NULL;
    lw_waiter_t *next_waiter;
    lw_uint32_t wait_list_count = 0;
    lw_waiter_id_t wait_list = LW_WAITER_ID_MAX;
    lw_waiter_id_t *wait_list_p = NULL;

    /* Threads waiting for the lock are appended to the waiter list. We Scan
     * through the waiter list looking for all threads that can be woken up.
     * We can wake up either one exclusive lock waiter, or all shared lock
     * waiters. However, to guarantee fairness, we only wake up shared lock
     * waiters blocked before the first exclusive lock waiter.
     *
     * Two possibilities after forming the list of waiters to wake up:
     *  1. The lock's waiter list became empty. If we lost the cmpxchg, the
     *     waiter list will be modified, and need to rescan the entire list.
     *  2. The lock's waiter list is non-empty, which means we now have two
     *     lists, one still associated with the lock, and another list of
     *     waiters we are about to wake up. Even if we lost the cmpxchg,
     *     the list of waiter to wake up will be unchanged, so we do not
     *     need to rescan through the list.
     */

    old = *rwlock;
    lw_assert(!old.lw_rwlock_unfair || exclusive);
    do {
        new = old;

        if (exclusive) {
            lw_assert(new.lw_rwlock_readers == 0 && new.lw_rwlock_wlocked);
            new.lw_rwlock_wlocked = 0;
        } else {
            lw_assert(new.lw_rwlock_readers > 0 && !new.lw_rwlock_wlocked);
            new.lw_rwlock_readers--;
        }
        lw_assert(new.lw_rwlock_waitq != LW_WAITER_ID_MAX);
        if (new.lw_rwlock_locked) {
            wait_list = LW_WAITER_ID_MAX;
        } else {
            /* wake up only the threads that will acquire the lock */

            if (wait_list_p == NULL) {
                /* form the list of waiters to wake up */
                wait_list_p = &new.lw_rwlock_waitq;
                wait_list_count = 1;

                waiter = lw_waiter_from_id(new.lw_rwlock_waitq);
                while (waiter->lw_waiter_next != LW_WAITER_ID_MAX) {
                    next_waiter = lw_waiter_from_id(waiter->lw_waiter_next);
                    if (waiter->lw_waiter_event.lw_te_base.lw_be_tag != LW_RWLOCK_SHARED ||
                        next_waiter->lw_waiter_event.lw_te_base.lw_be_tag != LW_RWLOCK_SHARED) {
                        /* exclusive waiter */
                        wait_list_p = &waiter->lw_waiter_next;
                        wait_list_count = 1;
                    } else {
                        /* shared waiter */
                        wait_list_count++;
                    }
                    waiter = next_waiter;
                }
                wait_list = *wait_list_p;
                lw_assert(lw_waiter_from_id(wait_list)->lw_waiter_event.lw_te_base.lw_be_tag == LW_RWLOCK_SHARED ||
                          wait_list_count == 1);

                *wait_list_p = LW_WAITER_ID_MAX;
                if (wait_list_p == &new.lw_rwlock_waitq) {
                    /* if we took over the entire list, need to rescan if we lose cmpxchg */
                    wait_list_p = NULL;
                }
            }

            if (waiter->lw_waiter_event.lw_te_base.lw_be_tag == LW_RWLOCK_SHARED) {
                /* waiters are waiting for shared lock */
#ifdef LW_DEBUG
                lw_uint32_t count = 0;
                lw_waiter_id_t id = wait_list;
                do {
                    waiter = lw_waiter_from_id(id);
                    lw_verify(waiter->lw_waiter_event.lw_te_base.lw_be_tag == LW_RWLOCK_SHARED);
                    count++;
                    id = waiter->lw_waiter_next;
                } while (id != LW_WAITER_ID_MAX);
                lw_verify(count == wait_list_count);
#endif
                lw_assert(exclusive);
                lw_assert(!new.lw_rwlock_unfair); /* only called for writer handoff for unfair locks. */
                new.lw_rwlock_readers = wait_list_count;
            } else {
                /* waiter is waiting for exclusive lock */
                lw_assert(wait_list_count == 1);
                lw_assert(lw_waiter_from_id(wait_list)->lw_waiter_event.lw_te_base.lw_be_tag !=
                          LW_RWLOCK_SHARED);
                lw_assert(lw_waiter_from_id(wait_list)->lw_waiter_next == LW_WAITER_ID_MAX);
                lw_assert(lw_waiter_from_id(wait_list) == waiter);
                new.lw_rwlock_wlocked = 1;
            }
        }

    } while (!lw_uint32_swap(&rwlock->lw_rwlock_val, &old.lw_rwlock_val, new.lw_rwlock_val));

    if (wait_list != LW_WAITER_ID_MAX) {
#ifdef LW_DEBUG
       lw_rwlock_check_waitq_membership(wait_list, rwlock->lw_rwlock_waitq);
#endif
        if (new.lw_rwlock_wlocked) {
            /* Lock was transferred to a write waiter */
            lw_assert(waiter != NULL);
            lw_assert(waiter->lw_waiter_event.lw_te_base.lw_be_tag != LW_RWLOCK_SHARED);
            lw_assert(wait_list_count == 1);
            lw_assert(wait_list == waiter->lw_waiter_id);
            lw_waiter_wakeup(waiter, rwlock);
        } else {
            lw_waiter_wake_all(lw_waiter_global_domain, wait_list, rwlock);
        }
    }
}

/* Downgrade exclusive lock to a shared lock, reset wait list.
 * Return current wait list.
 */
static lw_waiter_id_t
lw_rwlock_downgrade_and_ret_wait_list(LW_INOUT lw_rwlock_t *rwlock)
{
    lw_rwlock_t old;
    lw_rwlock_t new;
    lw_waiter_id_t wait_list;

    old.lw_rwlock_val = rwlock->lw_rwlock_val;
    do {
        lw_assert(old.lw_rwlock_readers == 0);
        lw_assert(old.lw_rwlock_wlocked == 1);
        new = old;
        new.lw_rwlock_readers = 1;
        new.lw_rwlock_wlocked = 0;
        wait_list = new.lw_rwlock_waitq;
        new.lw_rwlock_waitq = LW_WAITER_ID_MAX;
    } while (!lw_uint32_swap(&rwlock->lw_rwlock_val, &old.lw_rwlock_val, new.lw_rwlock_val));

    return wait_list;
}

static void
lw_rwlock_unlock_unfair_reinsert_waiters(LW_INOUT lw_rwlock_t *rwlock,
                                         LW_IN lw_uint32_t _writer_wait_list_to_insert,
                                         LW_INOUT lw_waiter_t *oldest_waiter_in_insert_list)
{
    lw_waiter_t *curr_waiter;
    lw_waiter_t *next_waiter;
    lw_waiter_id_t writer_wait_list_to_insert = _writer_wait_list_to_insert;

    lw_assert(oldest_waiter_in_insert_list != NULL);
    curr_waiter = lw_waiter_from_id(rwlock->lw_rwlock_waitq);
    while (curr_waiter->lw_waiter_next != LW_WAITER_ID_MAX) {
        lw_assert(curr_waiter->lw_waiter_event.lw_te_base.lw_be_tag != LW_RWLOCK_SHARED);
        next_waiter = lw_waiter_from_id(curr_waiter->lw_waiter_next);
        lw_assert(next_waiter->lw_waiter_event.lw_te_base.lw_be_tag != LW_RWLOCK_SHARED);
        if (next_waiter->lw_waiter_event.lw_te_base.lw_be_tag == LW_RWLOCK_UPGRADE) {
            /* One of the new readers went for an upgrade. We have
             * to hang the list before it.
             */
            lw_verify(next_waiter->lw_waiter_next == LW_WAITER_ID_MAX);
            lw_assert(next_waiter->lw_waiter_id == curr_waiter->lw_waiter_next);
            oldest_waiter_in_insert_list->lw_waiter_next = next_waiter->lw_waiter_id;
            curr_waiter->lw_waiter_next = writer_wait_list_to_insert;
            return;
        }
        curr_waiter = next_waiter;
    }

    if (curr_waiter->lw_waiter_event.lw_te_base.lw_be_tag == LW_RWLOCK_UPGRADE) {
        /* First waiter itself wants an upgrade */
        lw_verify(rwlock->lw_rwlock_waitq == curr_waiter->lw_waiter_id);
        lw_assert(curr_waiter->lw_waiter_next == LW_WAITER_ID_MAX);
        oldest_waiter_in_insert_list->lw_waiter_next = curr_waiter->lw_waiter_id;
        rwlock->lw_rwlock_waitq = writer_wait_list_to_insert;
        return;
    }
    /* We have the last waiter and want to add the waiter list in
     * front of it. However we have to be careful about a racing
     * upgrade getting ahead of us.
     */
    lw_assert(curr_waiter->lw_waiter_event.lw_te_base.lw_be_tag == LW_RWLOCK_EXCLUSIVE);
    if (lw_uint16_cmpxchg(&curr_waiter->lw_waiter_next,
                          LW_WAITER_ID_MAX,
                          writer_wait_list_to_insert) != LW_WAITER_ID_MAX) {
        /* Failed to swap. Someone else got in front due to an upgrade. */
        lw_assert(curr_waiter->lw_waiter_next != LW_WAITER_ID_MAX);
        next_waiter = lw_waiter_from_id(curr_waiter->lw_waiter_next);
        lw_assert(next_waiter->lw_waiter_next == LW_WAITER_ID_MAX);
        lw_assert(next_waiter->lw_waiter_event.lw_te_base.lw_be_tag == LW_RWLOCK_UPGRADE);
        /* Can do non-atomic update now as there can't be anymore races. */
        oldest_waiter_in_insert_list->lw_waiter_next = next_waiter->lw_waiter_id;
        curr_waiter->lw_waiter_next = writer_wait_list_to_insert;
    } /* else managed to swap. */
    return;
}

static void
lw_rwlock_unlock_unfair_contention(LW_INOUT lw_rwlock_t *rwlock)
{
    lw_rwlock_t old;
    lw_rwlock_t new;
    lw_waiter_t *waiter = NULL;
    lw_waiter_t *oldest_waiting_writer = NULL;
    lw_uint32_t readers_count = 0;
    lw_waiter_id_t reader_wait_list = LW_WAITER_ID_MAX;
    lw_waiter_id_t writer_wait_list = LW_WAITER_ID_MAX;
    lw_waiter_id_t wait_list = LW_WAITER_ID_MAX;

    /* This function is only called when holding writer lock.It transfers the
     * lock to the oldest waiter(s). If the oldest waiter is a writer, the lock
     * is given to it. If the oldest waiter is a reader, then *all* read waiters
     * are woken up.
     */
    old = *rwlock;
    lw_assert(old.lw_rwlock_unfair);
    lw_assert(old.lw_rwlock_readers == 0 && old.lw_rwlock_wlocked);
    lw_assert(old.lw_rwlock_waitq != LW_WAITER_ID_MAX);
    waiter = lw_waiter_from_id(old.lw_rwlock_waitq);
    while (waiter->lw_waiter_next != LW_WAITER_ID_MAX) { /* Not oldest waiter */
        waiter = lw_waiter_from_id(waiter->lw_waiter_next);
    }
    if (waiter->lw_waiter_event.lw_te_base.lw_be_tag != LW_RWLOCK_SHARED) {
        /* Last waiter is a writer. The lock needs to be transferred to
         * to the writer. We can use the lw_rwlock_unlock_fair_contention
         */
        lw_rwlock_unlock_fair_contention(rwlock, TRUE);
        return;
    }

    /* The last waiter is a reader. We need to wake up all readers. First
     * downgrade the lock to a reader lock so any incoming read lock attempts
     * don't end up queuing more waiters. That will ensure any new incoming
     * waiters will only be writers.
     */
    wait_list = lw_rwlock_downgrade_and_ret_wait_list(rwlock);
    lw_assert(wait_list != LW_WAITER_ID_MAX);
    /* Separate out the writers from the wait list */
    while ((waiter = lw_waiter_from_id(wait_list)) != NULL) {
        wait_list = waiter->lw_waiter_next;
        waiter->lw_waiter_next = LW_WAITER_ID_MAX;
        if (waiter->lw_waiter_event.lw_te_base.lw_be_tag == LW_RWLOCK_SHARED) {
            waiter->lw_waiter_next = reader_wait_list;
            reader_wait_list = waiter->lw_waiter_id;
            readers_count++;
        } else {
            /* Waiter is a writer. Remember it in the writer_wait_list
             * to re-add to the lock.
             */
            lw_assert(waiter->lw_waiter_event.lw_te_base.lw_be_tag == LW_RWLOCK_EXCLUSIVE); /* Can't be an upgrade waiter */
            if (oldest_waiting_writer == NULL) {
                lw_assert(writer_wait_list == LW_WAITER_ID_MAX);
                oldest_waiting_writer = waiter;
                writer_wait_list = waiter->lw_waiter_id;
            } else {
                lw_assert(writer_wait_list != LW_WAITER_ID_MAX);
                oldest_waiting_writer->lw_waiter_next = waiter->lw_waiter_id;
                oldest_waiting_writer = waiter;
            }
        }
    }

    lw_assert(reader_wait_list != LW_WAITER_ID_MAX &&
              readers_count > 0);
    lw_assert((oldest_waiting_writer == NULL &&
               writer_wait_list == LW_WAITER_ID_MAX) ||
              (oldest_waiting_writer != NULL &&
               writer_wait_list != LW_WAITER_ID_MAX));
    lw_assert(oldest_waiting_writer == NULL ||
              oldest_waiting_writer->lw_waiter_next == LW_WAITER_ID_MAX);
#ifdef LW_DEBUG
    lw_uint32_t count = 0;
    lw_waiter_id_t id = reader_wait_list;
    do {
        waiter = lw_waiter_from_id(id);
        lw_verify(waiter->lw_waiter_event.lw_te_base.lw_be_tag == LW_RWLOCK_SHARED);
        count++;
        id = waiter->lw_waiter_next;
    } while (id != LW_WAITER_ID_MAX);
    lw_verify(count == readers_count);
#endif

    /* Now we have seperated out the readers and writers. The readers will
     * get the lock and the writers need to go back on the wait list of the
     * lock.
     */
    old = *rwlock;
    do {
        new = old;
        lw_assert(old.lw_rwlock_readers >= 1 && !old.lw_rwlock_wlocked);
        new.lw_rwlock_readers += (readers_count - 1); /* -1 to subtract current thread */
        if (writer_wait_list != LW_WAITER_ID_MAX) {
            if (new.lw_rwlock_waitq == LW_WAITER_ID_MAX) {
                /* No new writers have come to wait. Set wait_id to
                 * writer_wait_list but keep the list in case this
                 * thread loses the cmpxchg.
                 */
                new.lw_rwlock_waitq = writer_wait_list;
            } else {
                /* Find oldest waiting writer and hang the writer_wait_list
                 * off of it.
                 */
                lw_rwlock_unlock_unfair_reinsert_waiters(&new, writer_wait_list, oldest_waiting_writer);
                if (new.lw_rwlock_waitq != writer_wait_list) {
                    /* Reset writer wait list since it has been re-inserted back. */
                    writer_wait_list = LW_WAITER_ID_MAX;
                } /* Else we need to hold on writer_wait_list in case cmpxchg fails. */
            }
        }
    } while (!lw_uint32_swap(&rwlock->lw_rwlock_val, &old.lw_rwlock_val, new.lw_rwlock_val));

    lw_waiter_wake_all(lw_waiter_global_domain, reader_wait_list, rwlock);
}

static lw_bool_t
try_rwlock_upgrade_or_release(LW_INOUT lw_rwlock_t *rwlock)
{
    lw_rwlock_t old;
    lw_rwlock_t new;
    lw_bool_t upgrade_res;

    /* This function is only called when holding unfair reader lock. It transfers the
     * lock to the oldest waiter which should neccessarily be a writer.
     */
    old.lw_rwlock_val = rwlock->lw_rwlock_val;
    lw_assert(old.lw_rwlock_unfair);
    lw_assert(old.lw_rwlock_readers > 0 && !old.lw_rwlock_wlocked);
    lw_assert(old.lw_rwlock_waitq != LW_WAITER_ID_MAX);
    do {
        upgrade_res = FALSE;
        lw_assert(old.lw_rwlock_readers > 0);
        lw_assert(old.lw_rwlock_wlocked == 0);

        new = old;
        new.lw_rwlock_readers--;
        if (!new.lw_rwlock_readers) {
            /* This is the only reader. Upgrade the read lock to a write lock.
             * Incoming read lock attempts will queue up as waiters.
             */
            new.lw_rwlock_wlocked = 1;
            upgrade_res = TRUE;
        }
    } while (!lw_uint32_swap(&rwlock->lw_rwlock_val, &old.lw_rwlock_val, new.lw_rwlock_val));

#ifdef LW_DEBUG
    lw_waiter_t *waiter = NULL;
    if (upgrade_res) {
        /* Verify last waiter is a writer if upgrade succeeded */
        old = *rwlock;
        lw_assert(old.lw_rwlock_readers == 0 && old.lw_rwlock_wlocked);
        lw_assert(old.lw_rwlock_waitq != LW_WAITER_ID_MAX);
        waiter = lw_waiter_from_id(old.lw_rwlock_waitq);
        while (waiter->lw_waiter_next != LW_WAITER_ID_MAX) { /* Not oldest waiter */
            waiter = lw_waiter_from_id(waiter->lw_waiter_next);
        }

        /* Last waiter should always be a writer */
        lw_assert(waiter->lw_waiter_event.lw_te_base.lw_be_tag != LW_RWLOCK_SHARED);
    }
#endif

    return upgrade_res;
}

/**
 * NOTE: do not invoke this function directly, use lw_rwlock_unlock() instead.
 * This function is invoked when lw_rwlock_unlock() runs into contention.
 */
static void
lw_rwlock_unlock_contention(LW_INOUT lw_rwlock_t *rwlock,
                            LW_IN lw_bool_t exclusive)
{
    lw_rwlock_t old;
    lw_bool_t upgrade_res;

    old = *rwlock;
    if (old.lw_rwlock_unfair && !exclusive) {
        /* This is an unfair shared lock.
         * Handle the case where a reader unlocking an unfair lock contends with
         * another thread that also grabs the read lock. If we dont handle this case
         * separately, it could result in a waiter on that lock getting removed
         * from the wait list and getting lost.
         * We upgrade the last reader to a writer and signal the oldest waiter.
         * If some other reader grabs the shared lock before we do the upgrade,
         * we only release the reader lock.
         */
        upgrade_res = try_rwlock_upgrade_or_release(rwlock);
        if (upgrade_res) {
            lw_rwlock_unlock_fair_contention(rwlock, TRUE);
        }
    } else if (!old.lw_rwlock_unfair) {
        /* This is a fair lock, simply call lw_rwlock_unlock_fair_contention. */
        lw_rwlock_unlock_fair_contention(rwlock, exclusive);
    } else {
        /* Unfair lock releasing writer lock. If the oldest waiter is a reader,
         * this needs to wake up all readers. If the oldest waiter is a writer,
         * then it needs to only wake up that writer.
         */
        lw_assert(old.lw_rwlock_unfair);
        lw_assert(exclusive);
        lw_rwlock_unlock_unfair_contention(rwlock);
    }
}

void
lw_rwlock_unlock(LW_INOUT lw_rwlock_t *rwlock,
                 LW_IN lw_bool_t exclusive)
{
    lw_rwlock_t old;
    lw_rwlock_t new;

    old.lw_rwlock_val = rwlock->lw_rwlock_val;
    do {
        new = old;

        if (exclusive) {
            lw_assert(new.lw_rwlock_readers == 0 && new.lw_rwlock_wlocked);
            new.lw_rwlock_wlocked = 0;
        } else {
            lw_assert(new.lw_rwlock_readers > 0 && !new.lw_rwlock_wlocked);
            new.lw_rwlock_readers--;
        }

        if (!new.lw_rwlock_locked && new.lw_rwlock_waitq != LW_WAITER_ID_MAX) {
            /* the lock is released, but there are threads in the wait list */
            lw_rwlock_unlock_contention(rwlock, exclusive);
            return;
        }

    } while (!lw_uint32_swap(&rwlock->lw_rwlock_val, &old.lw_rwlock_val, new.lw_rwlock_val));
}

void
lw_rwlock_downgrade(LW_INOUT lw_rwlock_t *rwlock)
{
    lw_rwlock_t old;
    lw_rwlock_t new;
    lw_waiter_t *last_waiter = NULL;
    lw_waiter_t *this_waiter = NULL;
    old = *rwlock;
    lw_assert(old.lw_rwlock_wlocked);
    lw_assert(old.lw_rwlock_readers == 0);
    do {
        new = old;
        if (old.lw_rwlock_waitq != LW_WAITER_ID_MAX) {
            /* Have existing waiters. Can't do direct downgrade */
            break;
        }
        new.lw_rwlock_wlocked = 0;
        new.lw_rwlock_readers = 1;
    } while (!lw_uint32_swap(&rwlock->lw_rwlock_val, &old.lw_rwlock_val, new.lw_rwlock_val));

    if (new.lw_rwlock_readers == 1) {
        /* Managed to do swap above */
        lw_assert(new.lw_rwlock_waitq == LW_WAITER_ID_MAX);
        return;
    } else {
        /* Swap didn't happen */
        lw_assert(new.lw_rwlock_val == old.lw_rwlock_val);
        lw_assert(new.lw_rwlock_wlocked);
        lw_assert(new.lw_rwlock_waitq != LW_WAITER_ID_MAX);
    }

    this_waiter = lw_waiter_get();
    last_waiter = lw_waiter_from_id(old.lw_rwlock_waitq);
    while (last_waiter->lw_waiter_next != LW_WAITER_ID_MAX) {
        last_waiter = lw_waiter_from_id(last_waiter->lw_waiter_next);
    }
    lw_assert(last_waiter != NULL);
    lw_assert(last_waiter->lw_waiter_next == LW_WAITER_ID_MAX);
    last_waiter->lw_waiter_next = this_waiter->lw_waiter_id;
    /* XXX: Is this safe to do? What if this thread already was using the
     * waiter for an async lock on another rwlock? We can do this downgrade
     * without using the waiter by simply doing the wakeup of all qualifying
     * waiters (readers, all or last depending upon fairness).
     *
     * Stuff below is simpler to do but limits the scope of where this feature
     * can be used.
     */
    lw_verify(this_waiter->lw_waiter_event.lw_te_base.lw_be_wait_src == NULL);
    lw_verify(this_waiter->lw_waiter_next == LW_WAITER_ID_MAX);
    this_waiter->lw_waiter_event.lw_te_base.lw_be_tag = LW_RWLOCK_SHARED;
    this_waiter->lw_waiter_event.lw_te_base.lw_be_wait_src = rwlock;
    lw_rwlock_unlock(rwlock, TRUE);
    lw_waiter_wait(this_waiter); /* Should wake up right away */
    old = *rwlock;
    lw_assert(old.lw_rwlock_readers > 0);
}

static int
lw_rwlock_insert_for_upgrade(LW_INOUT lw_rwlock_t *rwlock,
                             LW_INOUT lw_waiter_t *this_waiter)
{
    lw_waiter_t *last_waiter;

    while (TRUE) {
        lw_rwlock_t old = *rwlock;
        lw_assert(!old.lw_rwlock_wlocked);
        lw_assert(old.lw_rwlock_readers > 1 || old.lw_rwlock_waitq != LW_WAITER_ID_MAX);
        last_waiter = lw_waiter_from_id(old.lw_rwlock_waitq);
        while (last_waiter->lw_waiter_next != LW_WAITER_ID_MAX) {
            lw_assert(last_waiter->lw_waiter_event.lw_te_base.lw_be_tag != LW_RWLOCK_UPGRADE);
            last_waiter = lw_waiter_from_id(last_waiter->lw_waiter_next);
        }
        lw_assert(last_waiter != NULL);
        if (last_waiter->lw_waiter_event.lw_te_base.lw_be_tag == LW_RWLOCK_UPGRADE) {
            /* Someone else already waiting for upgrade */
            this_waiter->lw_waiter_event.lw_te_base.lw_be_wait_src = NULL;
            this_waiter->lw_waiter_event.lw_te_base.lw_be_tag = LW_RWLOCK_SHARED; /* Not really needed as tag is scratch space */
            return EPERM;
        }
        /* Try setting next pointer of last waiter.
         * Set the tag first since it needs to be visible to
         * any competing thread that is also trying the upgrade.
         */
        this_waiter->lw_waiter_event.lw_te_base.lw_be_tag = LW_RWLOCK_UPGRADE;
        this_waiter->lw_waiter_event.lw_te_base.lw_be_wait_src = rwlock;
        if (lw_uint16_cmpxchg(&last_waiter->lw_waiter_next,
                              LW_WAITER_ID_MAX,
                              this_waiter->lw_waiter_id) != LW_WAITER_ID_MAX) {
            /* Failed to swap. Someone else got in for upgrade */
            lw_assert(last_waiter->lw_waiter_next != LW_WAITER_ID_MAX);
            last_waiter = lw_waiter_from_id(last_waiter->lw_waiter_next);
            if (last_waiter->lw_waiter_event.lw_te_base.lw_be_tag == LW_RWLOCK_UPGRADE) {
                /* Lost to competing upgrade */
                this_waiter->lw_waiter_event.lw_te_base.lw_be_wait_src = NULL;
                this_waiter->lw_waiter_event.lw_te_base.lw_be_tag = LW_RWLOCK_SHARED; /* Not really needed as tag is scratch space */
                return EPERM;
            }
        } else {
            /* Managed to swap. this_waiter is in. */
            return 0;
        }
    }
}

int
lw_rwlock_upgrade(LW_INOUT lw_rwlock_t *rwlock)
{
    lw_rwlock_t old;
    lw_rwlock_t new;
    lw_waiter_t *this_waiter = NULL;
    old = *rwlock;
    lw_assert(!old.lw_rwlock_wlocked);
    lw_assert(old.lw_rwlock_readers != 0);
    this_waiter = lw_waiter_get();
    lw_assert(this_waiter->lw_waiter_event.lw_te_base.lw_be_wait_src == NULL);
    lw_assert(this_waiter->lw_waiter_next == LW_WAITER_ID_MAX);
    do {
        new = old;
        if (old.lw_rwlock_waitq != LW_WAITER_ID_MAX) {
            /*
             * Have more readers or waiter which could be in upgrade
             * itself. Can't grab right away.
             */
            break;
        } else if (old.lw_rwlock_readers == 1) {
            /* Only reader. Can grab it immediately */
            new.lw_rwlock_wlocked = 1;
            new.lw_rwlock_readers = 0;
        } else {
            this_waiter->lw_waiter_event.lw_te_base.lw_be_tag = LW_RWLOCK_UPGRADE;
            this_waiter->lw_waiter_event.lw_te_base.lw_be_wait_src = rwlock;
            new.lw_rwlock_waitq = this_waiter->lw_waiter_id;
            new.lw_rwlock_readers -= 1;
        }
    } while (!lw_uint32_swap(&rwlock->lw_rwlock_val, &old.lw_rwlock_val, new.lw_rwlock_val));

    if (new.lw_rwlock_wlocked) {
        /* Managed to do swap above */
        this_waiter->lw_waiter_event.lw_te_base.lw_be_wait_src = NULL;
        lw_assert(old.lw_rwlock_waitq == LW_WAITER_ID_MAX);
        lw_assert(new.lw_rwlock_waitq == LW_WAITER_ID_MAX);
        return 0;
    } else if (new.lw_rwlock_waitq != this_waiter->lw_waiter_id) {
        /* Swap didn't happen */
        int insert;
        lw_assert(new.lw_rwlock_val == old.lw_rwlock_val);
        lw_assert(!new.lw_rwlock_wlocked);
        lw_assert(new.lw_rwlock_readers > 1 || new.lw_rwlock_waitq != LW_WAITER_ID_MAX);
        insert = lw_rwlock_insert_for_upgrade(rwlock, this_waiter);
        if (insert != 0) {
            lw_assert(insert == EPERM);
            return EPERM;
        }
        lw_rwlock_unlock(rwlock, FALSE);
    }
    lw_assert(this_waiter->lw_waiter_event.lw_te_base.lw_be_tag == LW_RWLOCK_UPGRADE);
    lw_assert(this_waiter->lw_waiter_event.lw_te_base.lw_be_wait_src == rwlock);
    lw_waiter_wait(this_waiter);
    old = *rwlock;
    lw_assert(old.lw_rwlock_wlocked);

    return 0;
}
