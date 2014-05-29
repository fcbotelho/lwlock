/***
 *
 * Independently contributed to the lwlock library. This file is also released under the
 * terms of MPL 2.0 license but you do not need to sign the committer agreement for
 * this file.
 */

#include "lw_debug.h"
#include "lw_atomic.h"
#include "lw_bitlock.h"
#include "lw_waiter.h"
#include "murmur.h"
#include <errno.h>

#define MURMUR_SEED                     (0xa0f12ab7)    /* Something random. */
#define LW_BITLOCK_PTR_HASH32(_ptr)     Murmur3Ptr(_ptr, MURMUR_SEED)

/*
 * Bitlocks: 2 bits out of a 32 bit or a 64 bit value can be designated as the lock
 * and wait bits. The APIs of this module then provided the necessary mechanism to make
 * them work as a mutex.
 *
 * The lock bit is set when the lock is acquired. The wait bit is set if the lock cannot
 * be acquired right away. There is a common array of wait lists on which a thread waits
 * for the lock to be granted to it. The thread releasing the lock will wake up the waiting
 * thread.
 */

static lw_dlist_t *wait_lists = NULL;
static lw_uint32_t wait_lists_count = 1024; // Default value.
static lw_bool_t internal_alloc = FALSE;

/**
 * Init bitlock module.
 *
 * This function has to be called before any bitlocks can be operated on. It initializes
 * the lists where threads wait in contention case.
 *
 * @param num_wait_lists (i) the number of wait lists to use.
 * @param wait_list_memory (i) if !NULL, this is used for the lists. The caller has to
 * ensure the region is large enough for the number of lists desired.
 */
void
lw_bitlock_module_init(lw_uint32_t num_wait_lists, void *wait_list_memory)
{
    lw_uint32_t i;
    if (num_wait_lists != 0) {
        wait_lists_count = num_wait_lists;
    }
    if (wait_list_memory == NULL) {
        internal_alloc = TRUE;
        wait_list_memory = (lw_dlist_t *)malloc(sizeof(lw_dlist_t) * wait_lists_count);
    }
    wait_lists = wait_list_memory;
    for (i = 0; i < wait_lists_count; i++) {
        lw_dl_init(&wait_lists[i]);
    }
}

void
lw_bitlock_module_deinit(void)
{
    lw_uint32_t i;
    for (i = 0; i < wait_lists_count; i++) {
        lw_dl_destroy(&wait_lists[i]);
    }
    if (internal_alloc) {
        free(wait_lists);
    }
}

static lw_bool_t
lw_bitlock32_set_lock_bit(lw_uint32_t *lock,
                          lw_uint32_t lock_mask,
                          lw_uint32_t wait_mask,
                          lw_bool_t set_wait_bit)
{
    lw_uint32_t old, new;
    lw_uint32_t payload_mask = ~(lock_mask | wait_mask);
    old = 0;

    if (!set_wait_bit) {
        return lw_uint32_swap_with_mask(lock, payload_mask, &old, lock_mask);
    }

    do {
        if (old & lock_mask) {
            /* Lock already set. */
            new = lock_mask | wait_mask;
        } else {
            new = lock_mask;
            lw_assert(!(old & wait_mask));
        }
    } while (!lw_uint32_swap_with_mask(lock, payload_mask, &old, new));

    lw_assert(new & lock_mask);
    return (!(old & lock_mask));
}

static lw_bool_t
lw_bitlock32_drop_lock_if_no_waiters(lw_uint32_t *lock,
                                     lw_uint32_t lock_mask,
                                     lw_uint32_t wait_mask)
{
    lw_uint32_t payload_mask = ~(lock_mask | wait_mask);
    lw_uint32_t old = lock_mask;
    return lw_uint32_swap_with_mask(lock, payload_mask, &old, 0);
}

static void
lw_bitlock32_clear_wait_mask(lw_uint32_t *lock,
                             lw_uint32_t lock_mask,
                             lw_uint32_t wait_mask)
{
    lw_uint32_t old = (lock_mask | wait_mask);
    lw_uint32_t new = lock_mask;
    lw_uint32_t payload_mask = ~old;
    lw_bool_t cleared = lw_uint32_swap_with_mask(lock, payload_mask, &old, new);
    lw_assert(cleared);
}

/**
 * Init a bitlock. Clears the lock and wait bit.
 *
 * @param lock (i/o) the 32-bit word to init as a lock.
 * @param lock_mask (i) the bit that represents lock being held.
 * @param wait_mask (i) the bit that is set when waiting.
 */
void
lw_bitlock32_init(lw_uint32_t *lock,
                  LW_IN lw_uint32_t lock_mask,
                  LW_IN lw_uint32_t wait_mask)
{
    lw_uint32_t mask = lock_mask | wait_mask;
    *lock = *lock & ~mask;
}

/**
 * Destroy a bitlock. Verify that the lock and wait bit are clear.
 *
 * @param lock (i/o) the 32-bit word used as the lock.
 * @param lock_mask (i) the bit that represents lock being held.
 * @param wait_mask (i) the bit that is set when waiting.
 */
void
lw_bitlock32_destroy(lw_uint32_t *lock,
                     LW_IN lw_uint32_t lock_mask,
                     LW_IN lw_uint32_t wait_mask)
{
    lw_uint32_t mask = lock_mask | wait_mask;
    lw_verify((*lock & mask) == 0);
}

/**
 * Acquire a bitlock. Return whether the caller got the lock right away (TRUE)
 * or not (FALSE, needs to call lw_bitlock64_lock_complete_wait later).
 *
 * @param lock (i/o) the 32-bit word that holds the bits that form the lock.
 * @param lock_mask (i) the bit that represents lock being held.
 * @param wait_mask (i) the bit that is set when waiting.
 */
lw_bool_t
lw_bitlock32_lock_async(lw_uint32_t *lock,
                        LW_IN lw_uint32_t lock_mask,
                        LW_IN lw_uint32_t wait_mask)
{
    lw_uint32_t wait_list_idx;
    lw_dlist_t *wait_list;
    lw_waiter_t *waiter;
    lw_bool_t got_lock;

    lw_assert(lock_mask != wait_mask);
    lw_assert(lock_mask != 0 && wait_mask != 0);
    lw_assert(LW_IS_POW2(lock_mask) && LW_IS_POW2(wait_mask));

    got_lock = lw_bitlock32_set_lock_bit(lock, lock_mask, wait_mask, FALSE);
    if (got_lock) {
        /* All done. */
        return TRUE;
    }
    wait_list_idx = LW_BITLOCK_PTR_HASH32(lock);
    wait_list_idx = wait_list_idx % wait_lists_count;
    wait_list = &wait_lists[wait_list_idx];
    waiter = lw_waiter_get();
    lw_dl_lock_writer(wait_list);
    got_lock = lw_bitlock32_set_lock_bit(lock, lock_mask, wait_mask, TRUE);
    if (got_lock) {
        lw_dl_unlock_writer(wait_list);
        return TRUE;
    }
    lw_waiter_set_src(waiter, lock);
    waiter->event.tag = (lock_mask | wait_mask);
    lw_dl_append_at_end(wait_list, &waiter->event.iface.link);
    lw_dl_unlock_writer(wait_list);
    return FALSE;
}

/**
 * Acquire a bitlock if the payload matches. The paylaod check can only be done up to
 * the point of setting the wait bit. It is the users responsibility to ensure that the
 * payload will not change if either the lock or wait is set.
 *
 * @param lock (i/o) the 32-bit word that holds the bits that form the lock.
 * @param lock_mask (i) the bit that represents lock being held.
 * @param wait_mask (i) the bit that is set when waiting.
 */
lw_bool_t
lw_bitlock32_lock_if_payload(lw_uint32_t *lock,
                             LW_IN lw_uint32_t lock_mask,
                             LW_IN lw_uint32_t wait_mask,
                             lw_uint32_t *payload,
                             LW_IN lw_bool_t sync)
{
    lw_uint32_t wait_list_idx;
    lw_dlist_t *wait_list;
    lw_waiter_t *waiter;
    lw_uint32_t old, new, payload_mask;
    lw_bool_t got_lock;
    payload_mask = ~(lock_mask | wait_mask);
    *payload = *payload & payload_mask;

    lw_assert(lock_mask != wait_mask);
    lw_assert(lock_mask != 0 && wait_mask != 0);
    lw_assert(LW_IS_POW2(lock_mask) && LW_IS_POW2(wait_mask));

    old = *payload;
    new = *payload | lock_mask;
    got_lock = lw_uint32_swap(lock, &old, new);
    if (got_lock) {
        /* All done. */
        return TRUE;
    }
    if ((old & payload_mask) != *payload) {
        /* Different payload. */
        *payload = (old & payload_mask);
        return FALSE;
    }

    wait_list_idx = LW_BITLOCK_PTR_HASH32(lock);
    wait_list_idx = wait_list_idx % wait_lists_count;
    wait_list = &wait_lists[wait_list_idx];
    waiter = lw_waiter_get();
    lw_dl_lock_writer(wait_list);
    old = *payload;
    do {
        if ((old & payload_mask) != *payload) {
            /* Different payload. */
            lw_dl_unlock_writer(wait_list);
            *payload = (old & payload_mask);
            return FALSE;
        }
        if ((old & lock_mask) == 0) {
            lw_assert((old & wait_mask) == 0);
            new = old | lock_mask;
            got_lock = TRUE;
        } else {
            new = old | wait_mask;
            got_lock = FALSE;
        }
    } while (!lw_uint32_swap(lock, &old, new));
    if (got_lock) {
        lw_dl_unlock_writer(wait_list);
        return TRUE;
    }
    lw_waiter_set_src(waiter, lock);
    waiter->event.tag = (lock_mask | wait_mask);
    lw_dl_append_at_end(wait_list, &waiter->event.iface.link);
    lw_dl_unlock_writer(wait_list);
    if (sync) {
        lw_waiter_wait(waiter);
        lw_waiter_clear_src(waiter);
    }
    return TRUE;
}

/**
 * Try to acquire a bitlock.
 *
 * @param lock (i/o) the 32-bit word that holds the bits that form the lock.
 * @param lock_mask (i) the bit that represents lock being held.
 * @param wait_mask (i) the bit that is set when waiting.
 * @results 0 if lock is acquired, EBUSY if it cannot due to contention.
 */
lw_int32_t
lw_bitlock32_trylock(lw_uint32_t *lock, lw_uint32_t lock_mask, lw_uint32_t wait_mask)
{
    lw_assert(lock_mask != wait_mask);
    lw_assert(lock_mask != 0 && wait_mask != 0);
    lw_assert(LW_IS_POW2(lock_mask) && LW_IS_POW2(wait_mask));

    if (lw_bitlock32_set_lock_bit(lock, lock_mask, wait_mask, FALSE)) {
        return 0;
    }
    return EBUSY;
}

/**
 * Try to acquire a bitlock only if the payload matches the current value.
 * The curr_payload is updated on mismatch regardless of whether the lock failed due to
 * it or due to it being already locked.
 *
 * @param lock (i/o) the 32-bit word that holds the bits that form the lock.
 * @param lock_mask (i) the bit that represents lock being held.
 * @param wait_mask (i) the bit that is set when waiting.
 * @param curr_payload (i/o) the expected payload value. Updated to actual value.
 * @param new_payload (i) the new payload value to set.
 * @results 0 if lock is acquired, EBUSY if it cannot due to contention.
 */
lw_int32_t
lw_bitlock32_trylock_cmpxchng_payload(lw_uint32_t *lock,
                                      LW_IN lw_uint32_t lock_mask,
                                      LW_IN lw_uint32_t wait_mask,
                                      LW_INOUT lw_uint32_t *curr_payload,
                                      LW_IN lw_uint32_t new_payload)
{
    lw_uint32_t new, old;
    lw_bool_t swapped = FALSE;

    lw_assert(lock_mask != wait_mask);
    lw_assert(lock_mask != 0 && wait_mask != 0);
    lw_assert(LW_IS_POW2(lock_mask) && LW_IS_POW2(wait_mask));
    lw_assert((lock_mask & *curr_payload) == 0);
    lw_assert((wait_mask & *curr_payload) == 0);
    lw_assert((lock_mask & new_payload) == 0);
    lw_assert((wait_mask & new_payload) == 0);

    old = *curr_payload;
    new = new_payload | lock_mask;
    swapped = lw_uint32_swap(lock, &old, new);
    *curr_payload = old & ~(lock_mask | wait_mask);
    return swapped ? 0 : EBUSY;
}

static lw_waiter_t *
lw_bitlock32_get_one_waiter(lw_dlist_t *wait_list,
                            lw_uint32_t *lock,
                            lw_uint32_t lock_mask,
                            lw_uint32_t wait_mask)
{
    lw_waiter_t *to_wake_up = NULL;
    lw_delem_t *elem;
    lw_uint32_t mask = lock_mask | wait_mask;
    lw_bool_t multiple_waiters = FALSE;

    lw_assert(lock_mask != wait_mask);
    lw_assert(lock_mask != 0 && wait_mask != 0);
    lw_assert(LW_IS_POW2(lock_mask) && LW_IS_POW2(wait_mask));

    lw_assert(wait_list == &wait_lists[LW_BITLOCK_PTR_HASH32(lock) % wait_lists_count]);
    lw_assert(lw_dl_trylock_writer(wait_list) == EBUSY);

    elem = wait_list->head;
    while (elem != NULL) {
        lw_waiter_t *waiter = LW_FIELD_2_OBJ_NULL_SAFE(elem, *waiter, event.iface.link);
        if (waiter->event.wait_src == lock && waiter->event.tag == mask) {
            /* Found a waiter. */
            if (to_wake_up == NULL) {
                to_wake_up = waiter;
            } else {
                multiple_waiters = TRUE;
                break;
            }
        }
        elem = lw_dl_next(wait_list, elem);
    }
    lw_assert(to_wake_up != NULL);
    lw_dl_remove(wait_list, &to_wake_up->event.iface.link);
    if (!multiple_waiters)  {
        /* Clear wait bit while holding wait list lock to prevent new waiters from setting it again. */
        lw_bitlock32_clear_wait_mask(lock, lock_mask, wait_mask);
    }
    return to_wake_up;
}

/**
 * Release a bitlock. If there are waiters, the 1st one is woken up and the lock handed over
 * to it.
 *
 * @param lock (i/o) the 32-bit word that holds the bits that form the lock.
 * @param lock_mask (i) the bit that represents lock being held.
 * @param wait_mask (i) the bit that is set when waiting.
 * @returns TRUE if the lock was handed off to a waiter. FALSE otherwise.
 */
lw_waiter_t *
lw_bitlock32_unlock_return_waiter(lw_uint32_t *lock,
                                  LW_IN lw_uint32_t lock_mask,
                                  LW_IN lw_uint32_t wait_mask)
{
    lw_uint32_t wait_list_idx;
    lw_dlist_t *wait_list;
    lw_waiter_t *to_wake_up = NULL;

    lw_assert(lock_mask != wait_mask);
    lw_assert(lock_mask != 0 && wait_mask != 0);
    lw_assert(LW_IS_POW2(lock_mask) && LW_IS_POW2(wait_mask));

    if (lw_bitlock32_drop_lock_if_no_waiters(lock, lock_mask, wait_mask)) {
        /* All done. */
        return NULL;
    }

    wait_list_idx = LW_BITLOCK_PTR_HASH32(lock);
    wait_list_idx = wait_list_idx % wait_lists_count;
    wait_list = &wait_lists[wait_list_idx];

    lw_dl_lock_writer(wait_list);
    to_wake_up = lw_bitlock32_get_one_waiter(wait_list, lock, lock_mask, wait_mask);
    lw_assert(to_wake_up != NULL);
    lw_dl_unlock_writer(wait_list);
    return to_wake_up;
}

/**
 * Safe routine to update the "payload" bits of a bit lock. Caller need not hold the
 * lock when calling this.
 *
 * @param lock (i/o) the 32-bit word that holds the bits that form the lock.
 * @param lock_mask (i) the bit that represents lock being held.
 * @param wait_mask (i) the bit that is set when waiting.
 * @param current_payload (i/o) the expected value of payload. Updated if swap fails.
 * @param new_payload (i) the new value of the payload should swap succeed.
 */
lw_bool_t
lw_bitlock32_swap_payload(lw_uint32_t *lock,
                          lw_uint32_t lock_mask,
                          lw_uint32_t wait_mask,
                          lw_uint32_t *current_payload,
                          lw_uint32_t new_payload)
{
    lw_uint32_t mask =  lock_mask | wait_mask;

    lw_assert(lock_mask != wait_mask);
    lw_assert(lock_mask != 0 && wait_mask != 0);
    lw_assert(LW_IS_POW2(lock_mask) && LW_IS_POW2(wait_mask));

    return lw_uint32_swap_with_mask(lock, mask, current_payload, new_payload);
}

static lw_bool_t
lw_bitlock64_set_lock_bit(lw_uint64_t *lock,
                          lw_uint64_t lock_mask,
                          lw_uint64_t wait_mask,
                          lw_bool_t set_wait_bit)
{
    lw_uint64_t old, new;
    lw_uint64_t payload_mask = ~(lock_mask | wait_mask);
    old = 0;

    if (!set_wait_bit) {
        return lw_uint64_swap_with_mask(lock, payload_mask, &old, lock_mask);
    }

    do {
        if (old & lock_mask) {
            /* Lock already set. */
            new = lock_mask | wait_mask;
        } else {
            new = lock_mask;
            lw_assert(!(old & wait_mask));
        }
    } while (!lw_uint64_swap_with_mask(lock, payload_mask, &old, new));

    lw_assert(new & lock_mask);
    return (!(old & lock_mask));
}

static lw_bool_t
lw_bitlock64_drop_lock_if_no_waiters(lw_uint64_t *lock,
                                     lw_uint64_t lock_mask,
                                     lw_uint64_t wait_mask)
{
    lw_uint64_t payload_mask = ~(lock_mask | wait_mask);
    lw_uint64_t old = lock_mask;
    return lw_uint64_swap_with_mask(lock, payload_mask, &old, 0);
}

static void
lw_bitlock64_clear_wait_mask(lw_uint64_t *lock,
                             lw_uint64_t lock_mask,
                             lw_uint64_t wait_mask)
{
    lw_uint64_t old = (lock_mask | wait_mask);
    lw_uint64_t new = lock_mask;
    lw_uint64_t payload_mask = ~old;
    lw_bool_t cleared = lw_uint64_swap_with_mask(lock, payload_mask, &old, new);
    lw_assert(cleared);
}

/**
 * Init a bitlock. Clears the lock and wait bit.
 *
 * @param lock (i/o) the 64-bit word to init as a lock.
 * @param lock_mask (i) the bit that represents lock being held.
 * @param wait_mask (i) the bit that is set when waiting.
 */
void
lw_bitlock64_init(lw_uint64_t *lock,
                  LW_IN lw_uint64_t lock_mask,
                  LW_IN lw_uint64_t wait_mask)
{
    lw_uint64_t mask = lock_mask | wait_mask;
    *lock = *lock & ~mask;
}

/**
 * Destroy a bitlock. Verify that the lock and wait bit are clear.
 *
 * @param lock (i/o) the 64-bit word used as the lock.
 * @param lock_mask (i) the bit that represents lock being held.
 * @param wait_mask (i) the bit that is set when waiting.
 */
void
lw_bitlock64_destroy(lw_uint64_t *lock,
                     LW_IN lw_uint64_t lock_mask,
                     LW_IN lw_uint64_t wait_mask)
{
    lw_uint64_t mask = lock_mask | wait_mask;
    lw_verify((*lock & mask) == 0);
}

/**
 * Acquire a bitlock. Return whether the caller got the lock right away (TRUE)
 * or not (FALSE, needs to call lw_bitlock64_lock_complete_wait later).
 *
 * @param lock (i/o) the 64-bit word that holds the bits that form the lock.
 * @param lock_mask (i) the bit that represents lock being held.
 * @param wait_mask (i) the bit that is set when waiting.
 */
lw_bool_t
lw_bitlock64_lock_async(lw_uint64_t *lock,
                        LW_IN lw_uint64_t lock_mask,
                        LW_IN lw_uint64_t wait_mask)
{
    lw_uint64_t wait_list_idx;
    lw_dlist_t *wait_list;
    lw_waiter_t *waiter;
    lw_bool_t got_lock;

    lw_assert(lock_mask != wait_mask);
    lw_assert(lock_mask != 0 && wait_mask != 0);
    lw_assert(LW_IS_POW2(lock_mask) && LW_IS_POW2(wait_mask));

    got_lock = lw_bitlock64_set_lock_bit(lock, lock_mask, wait_mask, FALSE);
    if (got_lock) {
        /* All done. */
        return TRUE;
    }
    wait_list_idx = LW_BITLOCK_PTR_HASH32(lock);
    wait_list_idx = wait_list_idx % wait_lists_count;
    wait_list = &wait_lists[wait_list_idx];
    waiter = lw_waiter_get();
    lw_dl_lock_writer(wait_list);
    got_lock = lw_bitlock64_set_lock_bit(lock, lock_mask, wait_mask, TRUE);
    if (got_lock) {
        lw_dl_unlock_writer(wait_list);
        return TRUE;
    }
    lw_waiter_set_src(waiter, lock);
    waiter->event.tag = (lock_mask | wait_mask);
    lw_dl_append_at_end(wait_list, &waiter->event.iface.link);
    lw_dl_unlock_writer(wait_list);
    return FALSE;
}

/**
 * Wait on the CV bit of the bitlock.
 *
 * @param lock (i/o) the 64-bit word that holds the bits that form the lock.
 * @param lock_mask (i) the bit that represents lock being held.
 * @param wait_mask (i) the bit that is set by waiters of the lock.
 * @param cv_mask (i) the bit that is set when doing cond wait.
 */
void
lw_bitlock32_cv_wait(lw_uint32_t *lock,
                     LW_IN lw_uint32_t lock_mask,
                     LW_IN lw_uint32_t wait_mask,
                     LW_IN lw_uint32_t cv_mask)
{
    lw_uint64_t wait_list_idx;
    lw_dlist_t *wait_list;
    lw_waiter_t *waiter;
    lw_waiter_t *to_wake_up = NULL;
    lw_uint32_t old;

    lw_assert(lock_mask != wait_mask);
    lw_assert(cv_mask != wait_mask);
    lw_assert(lock_mask != cv_mask);
    lw_assert(lock_mask != 0 && wait_mask != 0 && cv_mask != 0);
    lw_assert(LW_IS_POW2(lock_mask) && LW_IS_POW2(wait_mask) && LW_IS_POW2(cv_mask));
    lw_assert((*lock & lock_mask));

    old = 0;
    LW_IGNORE_RETURN_VALUE(lw_uint32_swap_with_mask(lock, ~cv_mask, &old, cv_mask));
    lw_assert(*lock & cv_mask);

    wait_list_idx = LW_BITLOCK_PTR_HASH32(lock);
    wait_list_idx = wait_list_idx % wait_lists_count;
    wait_list = &wait_lists[wait_list_idx];
    waiter = lw_waiter_get();
    lw_dl_lock_writer(wait_list);
    lw_waiter_set_src(waiter, lock);
    waiter->event.tag = (lock_mask | cv_mask);
    if (!lw_bitlock32_drop_lock_if_no_waiters(lock, lock_mask, wait_mask)) {
        to_wake_up = lw_bitlock32_get_one_waiter(wait_list, lock, lock_mask, wait_mask);
        lw_assert(to_wake_up != NULL);
    }
    lw_dl_append_at_end(wait_list, &waiter->event.iface.link);
    lw_dl_unlock_writer(wait_list);

    if (to_wake_up != NULL) {
        lw_waiter_wakeup(to_wake_up, lock);
    }
    /* Wait. */
    lw_waiter_wait(waiter);
    lw_waiter_clear_src(waiter);
    return;
}

/**
 * Signal the bit cv.
 *
 * @param lock (i/o) the 64-bit word that holds the bits that form the lock.
 * @param lock_mask (i) the bit that represents lock being held.
 * @param wait_mask (i) the bit that is set when waiting.
 * @param cv_mask (i) the bit that is set when doing cond wait.
 */
void
lw_bitlock32_cv_signal(lw_uint32_t *lock,
                       LW_IN lw_uint32_t lock_mask,
                       LW_IN lw_uint32_t wait_mask,
                       LW_IN lw_uint32_t cv_mask)
{
    lw_uint64_t wait_list_idx;
    lw_dlist_t *wait_list;
    lw_waiter_t *to_move = NULL;
    lw_delem_t *elem;
    lw_uint64_t cv_tag = lock_mask | cv_mask;
    lw_uint64_t wait_tag = lock_mask | wait_mask;
    lw_bool_t multiple_waiters = FALSE;
    lw_uint32_t old;

    lw_assert(lock_mask != wait_mask);
    lw_assert(cv_mask != wait_mask);
    lw_assert(lock_mask != cv_mask);
    lw_assert(lock_mask != 0 && wait_mask != 0 && cv_mask != 0);
    lw_assert(LW_IS_POW2(lock_mask) && LW_IS_POW2(wait_mask) && LW_IS_POW2(cv_mask));
    lw_assert((*lock & lock_mask));

    if ((*lock & cv_mask) == 0) {
        /* No one in a cv wait. */
        return;
    }

    wait_list_idx = LW_BITLOCK_PTR_HASH32(lock);
    wait_list_idx = wait_list_idx % wait_lists_count;
    wait_list = &wait_lists[wait_list_idx];

    lw_dl_lock_writer(wait_list);
    elem = wait_list->head;
    while (elem != NULL) {
        lw_waiter_t *waiter = LW_FIELD_2_OBJ_NULL_SAFE(elem, *waiter, event.iface.link);
        if (waiter->event.wait_src == lock && waiter->event.tag == cv_tag) {
            /* Found a waiter. */
            if (to_move == NULL) {
                to_move = waiter;
                to_move->event.tag = wait_tag;
            } else {
                multiple_waiters = TRUE;
                break;
            }
        }
        elem = lw_dl_next(wait_list, elem);
    }
    lw_assert(to_move != NULL);
    lw_assert(to_move->event.tag == wait_tag);
    lw_dl_unlock_writer(wait_list);
    if (!multiple_waiters)  {
        /* Clear cv bit. Lock is held, so no race can take place. */
        old = cv_mask;
        LW_IGNORE_RETURN_VALUE(lw_uint32_swap_with_mask(lock, ~cv_mask, &old, 0));
    }
    /* Set wait bit. */
    old = 0;
    LW_IGNORE_RETURN_VALUE(lw_uint32_swap_with_mask(lock, ~wait_mask, &old, wait_mask));
    return;
}

/**
 * Broadcast the bit cv.
 *
 * @param lock (i/o) the 64-bit word that holds the bits that form the lock.
 * @param lock_mask (i) the bit that represents lock being held.
 * @param wait_mask (i) the bit that is set when waiting.
 * @param cv_mask (i) the bit that is set when doing cond wait.
 */
void
lw_bitlock32_cv_broadcast(lw_uint32_t *lock,
                          LW_IN lw_uint32_t lock_mask,
                          LW_IN lw_uint32_t wait_mask,
                          LW_IN lw_uint32_t cv_mask)
{
    lw_uint64_t wait_list_idx;
    lw_dlist_t *wait_list;
    lw_delem_t *elem;
    lw_uint64_t cv_tag = lock_mask | cv_mask;
    lw_uint64_t wait_tag = lock_mask | wait_mask;
    lw_bool_t atleast_one = FALSE;
    lw_uint32_t old;

    lw_assert(lock_mask != wait_mask);
    lw_assert(cv_mask != wait_mask);
    lw_assert(lock_mask != cv_mask);
    lw_assert(lock_mask != 0 && wait_mask != 0 && cv_mask != 0);
    lw_assert(LW_IS_POW2(lock_mask) && LW_IS_POW2(wait_mask) && LW_IS_POW2(cv_mask));
    lw_assert((*lock & lock_mask));

    if ((*lock & cv_mask) == 0) {
        /* No one in a cv wait. */
        return;
    }

    wait_list_idx = LW_BITLOCK_PTR_HASH32(lock);
    wait_list_idx = wait_list_idx % wait_lists_count;
    wait_list = &wait_lists[wait_list_idx];

    lw_dl_lock_writer(wait_list);
    elem = wait_list->head;
    while (elem != NULL) {
        lw_waiter_t *waiter = LW_FIELD_2_OBJ_NULL_SAFE(elem, *waiter, event.iface.link);
        if (waiter->event.wait_src == lock && waiter->event.tag == cv_tag) {
            /* Found a waiter. */
            atleast_one = TRUE;
            waiter->event.tag = wait_tag;
        }
        elem = lw_dl_next(wait_list, elem);
    }
    lw_assert(atleast_one);
    LW_UNUSED_PARAMETER(atleast_one);
    lw_dl_unlock_writer(wait_list);
    /* Clear cv bit. Lock is held, so no race can take place. Also set wait bit. */
    old = cv_mask;
    LW_IGNORE_RETURN_VALUE(lw_uint32_swap_with_mask(lock, ~cv_mask, &old, 0));
    old = 0;
    LW_IGNORE_RETURN_VALUE(lw_uint32_swap_with_mask(lock, ~wait_mask, 0, old));
    return;
}

/**
 * Acquire a bitlock if the payload matches. The paylaod check can only be done up to
 * the point of setting the wait bit. It is the users responsibility to ensure that the
 * payload will not change if either the lock or wait is set.
 *
 * @param lock (i/o) the 64-bit word that holds the bits that form the lock.
 * @param lock_mask (i) the bit that represents lock being held.
 * @param wait_mask (i) the bit that is set when waiting.
 */
lw_bool_t
lw_bitlock64_lock_if_payload(lw_uint64_t *lock,
                             LW_IN lw_uint64_t lock_mask,
                             LW_IN lw_uint64_t wait_mask,
                             lw_uint64_t *payload,
                             LW_IN lw_bool_t sync)
{
    lw_uint64_t wait_list_idx;
    lw_dlist_t *wait_list;
    lw_waiter_t *waiter;
    lw_uint64_t old, new, payload_mask;
    lw_bool_t got_lock;
    payload_mask = ~(lock_mask | wait_mask);
    *payload = *payload & payload_mask;

    lw_assert(lock_mask != wait_mask);
    lw_assert(lock_mask != 0 && wait_mask != 0);
    lw_assert(LW_IS_POW2(lock_mask) && LW_IS_POW2(wait_mask));

    old = *payload;
    new = *payload | lock_mask;
    got_lock = lw_uint64_swap(lock, &old, new);
    if (got_lock) {
        /* All done. */
        return TRUE;
    }
    if ((old & payload_mask) != *payload) {
        /* Different payload. */
        *payload = old & payload_mask;
        return FALSE;
    }

    wait_list_idx = LW_BITLOCK_PTR_HASH32(lock);
    wait_list_idx = wait_list_idx % wait_lists_count;
    wait_list = &wait_lists[wait_list_idx];
    waiter = lw_waiter_get();
    lw_dl_lock_writer(wait_list);
    old = *payload;
    do {
        if ((old & payload_mask) != *payload) {
            /* Different payload. */
            lw_dl_unlock_writer(wait_list);
            *payload = old & payload_mask;
            return FALSE;
        }
        if ((old & lock_mask) == 0) {
            lw_assert((old & wait_mask) == 0);
            new = old | lock_mask;
            got_lock = TRUE;
        } else {
            new = old | wait_mask;
            got_lock = FALSE;
        }
    } while (!lw_uint64_swap(lock, &old, new));
    if (got_lock) {
        lw_dl_unlock_writer(wait_list);
        return TRUE;
    }
    lw_waiter_set_src(waiter, lock);
    waiter->event.tag = (lock_mask | wait_mask);
    lw_dl_append_at_end(wait_list, &waiter->event.iface.link);
    lw_dl_unlock_writer(wait_list);
    if (sync) {
        lw_waiter_wait(waiter);
        lw_waiter_clear_src(waiter);
    }
    return TRUE;
}

void
lw_bitlock_complete_wait(void *lock)
{
    lw_waiter_t *waiter = lw_waiter_get();
    if (waiter->event.wait_src == lock) {
        lw_waiter_wait(waiter);
        lw_waiter_clear_src(waiter);
    }
    lw_waiter_assert_src(waiter, NULL);
}

/**
 * Try to acquire a bitlock.
 *
 * @param lock (i/o) the 64-bit word that holds the bits that form the lock.
 * @param lock_mask (i) the bit that represents lock being held.
 * @param wait_mask (i) the bit that is set when waiting.
 * @results 0 if lock is acquired, EBUSY if it cannot due to contention.
 */
lw_int32_t
lw_bitlock64_trylock(lw_uint64_t *lock, lw_uint64_t lock_mask, lw_uint64_t wait_mask)
{
    lw_assert(lock_mask != wait_mask);
    lw_assert(lock_mask != 0 && wait_mask != 0);
    lw_assert(LW_IS_POW2(lock_mask) && LW_IS_POW2(wait_mask));

    if (lw_bitlock64_set_lock_bit(lock, lock_mask, wait_mask, FALSE)) {
        return 0;
    }
    return EBUSY;
}

/**
 * Try to acquire a bitlock only if the payload matches the current value.
 * The curr_payload is updated on mismatch regardless of whether the lock failed due to
 * it or due to it being already locked.
 *
 * @param lock (i/o) the 64-bit word that holds the bits that form the lock.
 * @param lock_mask (i) the bit that represents lock being held.
 * @param wait_mask (i) the bit that is set when waiting.
 * @param curr_payload (i/o) the expected payload value. Updated to actual value.
 * @param new_payload (i) the new payload value to set.
 * @results 0 if lock is acquired, EBUSY if it cannot due to contention.
 */
lw_int32_t
lw_bitlock64_trylock_cmpxchng_payload(lw_uint64_t *lock,
                                      LW_IN lw_uint64_t lock_mask,
                                      LW_IN lw_uint64_t wait_mask,
                                      LW_INOUT lw_uint64_t *curr_payload,
                                      LW_IN lw_uint64_t new_payload)
{
    lw_uint64_t new, old;
    lw_bool_t swapped = FALSE;

    lw_assert(lock_mask != wait_mask);
    lw_assert(lock_mask != 0 && wait_mask != 0);
    lw_assert(LW_IS_POW2(lock_mask) && LW_IS_POW2(wait_mask));
    lw_assert((lock_mask & *curr_payload) == 0);
    lw_assert((wait_mask & *curr_payload) == 0);
    lw_assert((lock_mask & new_payload) == 0);
    lw_assert((wait_mask & new_payload) == 0);

    old = *curr_payload;
    new = new_payload | lock_mask;
    swapped = lw_uint64_swap(lock, &old, new);
    *curr_payload = old & ~(lock_mask | wait_mask);
    return swapped ? 0 : EBUSY;
}

static lw_waiter_t *
lw_bitlock64_get_one_waiter(lw_dlist_t *wait_list,
                            lw_uint64_t *lock,
                            lw_uint64_t lock_mask,
                            lw_uint64_t wait_mask)
{
    lw_waiter_t *to_wake_up = NULL;
    lw_delem_t *elem;
    lw_uint64_t mask = lock_mask | wait_mask;
    lw_bool_t multiple_waiters = FALSE;

    lw_assert(lock_mask != wait_mask);
    lw_assert(lock_mask != 0 && wait_mask != 0);
    lw_assert(LW_IS_POW2(lock_mask) && LW_IS_POW2(wait_mask));

    lw_assert(wait_list == &wait_lists[LW_BITLOCK_PTR_HASH32(lock) % wait_lists_count]);
    lw_assert(lw_dl_trylock_writer(wait_list) == EBUSY);

    elem = wait_list->head;
    while (elem != NULL) {
        lw_waiter_t *waiter = LW_FIELD_2_OBJ_NULL_SAFE(elem, *waiter, event.iface.link);
        if (waiter->event.wait_src == lock && waiter->event.tag == mask) {
            /* Found a waiter. */
            if (to_wake_up == NULL) {
                to_wake_up = waiter;
            } else {
                multiple_waiters = TRUE;
                break;
            }
        }
        elem = lw_dl_next(wait_list, elem);
    }
    lw_assert(to_wake_up != NULL);
    lw_dl_remove(wait_list, &to_wake_up->event.iface.link);
    if (!multiple_waiters)  {
        /* Clear wait bit while holding wait list lock to prevent new waiters from setting it again. */
        lw_bitlock64_clear_wait_mask(lock, lock_mask, wait_mask);
    }
    return to_wake_up;
}

/**
 * Release a bitlock. If there are waiters, the 1st one is woken up and the lock handed over
 * to it.
 *
 * @param lock (i/o) the 64-bit word that holds the bits that form the lock.
 * @param lock_mask (i) the bit that represents lock being held.
 * @param wait_mask (i) the bit that is set when waiting.
 * @returns TRUE if the lock was handed off to a waiter. FALSE otherwise.
 */
lw_waiter_t *
lw_bitlock64_unlock_return_waiter(lw_uint64_t *lock,
                                  LW_IN lw_uint64_t lock_mask,
                                  LW_IN lw_uint64_t wait_mask)
{
    lw_uint64_t wait_list_idx;
    lw_dlist_t *wait_list;
    lw_waiter_t *to_wake_up = NULL;

    lw_assert(lock_mask != wait_mask);
    lw_assert(lock_mask != 0 && wait_mask != 0);
    lw_assert(LW_IS_POW2(lock_mask) && LW_IS_POW2(wait_mask));

    if (lw_bitlock64_drop_lock_if_no_waiters(lock, lock_mask, wait_mask)) {
        /* All done. */
        return NULL;
    }

    wait_list_idx = LW_BITLOCK_PTR_HASH32(lock);
    wait_list_idx = wait_list_idx % wait_lists_count;
    wait_list = &wait_lists[wait_list_idx];

    lw_dl_lock_writer(wait_list);
    to_wake_up = lw_bitlock64_get_one_waiter(wait_list, lock, lock_mask, wait_mask);
    lw_assert(to_wake_up != NULL);
    lw_dl_unlock_writer(wait_list);
    return to_wake_up;
}

/**
 * Safe routine to update the "payload" bits of a bit lock. Caller need not hold the
 * lock when calling this.
 *
 * @param lock (i/o) the 64-bit word that holds the bits that form the lock.
 * @param lock_mask (i) the bit that represents lock being held.
 * @param wait_mask (i) the bit that is set when waiting.
 * @param current_payload (i/o) the expected value of payload. Updated if swap fails.
 * @param new_payload (i) the new value of the payload should swap succeed.
 */
lw_bool_t
lw_bitlock64_swap_payload(lw_uint64_t *lock,
                          lw_uint64_t lock_mask,
                          lw_uint64_t wait_mask,
                          lw_uint64_t *current_payload,
                          lw_uint64_t new_payload)
{
    lw_uint64_t mask =  lock_mask | wait_mask;

    lw_assert(lock_mask != wait_mask);
    lw_assert(lock_mask != 0 && wait_mask != 0);
    lw_assert(LW_IS_POW2(lock_mask) && LW_IS_POW2(wait_mask));

    return lw_uint64_swap_with_mask(lock, mask, current_payload, new_payload);
}

/**
 * Wait on the CV bit of the bitlock.
 *
 * @param lock (i/o) the 64-bit word that holds the bits that form the lock.
 * @param lock_mask (i) the bit that represents lock being held.
 * @param wait_mask (i) the bit that is set by waiters of the lock.
 * @param cv_mask (i) the bit that is set when doing cond wait.
 */
void
lw_bitlock64_cv_wait(lw_uint64_t *lock,
                     LW_IN lw_uint64_t lock_mask,
                     LW_IN lw_uint64_t wait_mask,
                     LW_IN lw_uint64_t cv_mask)
{
    lw_uint64_t wait_list_idx;
    lw_dlist_t *wait_list;
    lw_waiter_t *waiter;
    lw_waiter_t *to_wake_up = NULL;
    lw_uint64_t old;

    lw_assert(lock_mask != wait_mask);
    lw_assert(cv_mask != wait_mask);
    lw_assert(lock_mask != cv_mask);
    lw_assert(lock_mask != 0 && wait_mask != 0 && cv_mask != 0);
    lw_assert(LW_IS_POW2(lock_mask) && LW_IS_POW2(wait_mask) && LW_IS_POW2(cv_mask));
    lw_assert((*lock & lock_mask));

    old = 0;
    LW_IGNORE_RETURN_VALUE(lw_uint64_swap_with_mask(lock, ~cv_mask, &old, cv_mask));
    lw_assert(*lock & cv_mask);

    wait_list_idx = LW_BITLOCK_PTR_HASH32(lock);
    wait_list_idx = wait_list_idx % wait_lists_count;
    wait_list = &wait_lists[wait_list_idx];
    waiter = lw_waiter_get();
    lw_dl_lock_writer(wait_list);
    lw_waiter_set_src(waiter, lock);
    waiter->event.tag = (lock_mask | cv_mask);
    if (!lw_bitlock64_drop_lock_if_no_waiters(lock, lock_mask, wait_mask)) {
        to_wake_up = lw_bitlock64_get_one_waiter(wait_list, lock, lock_mask, wait_mask);
        lw_assert(to_wake_up != NULL);
    }
    lw_dl_append_at_end(wait_list, &waiter->event.iface.link);
    lw_dl_unlock_writer(wait_list);

    if (to_wake_up != NULL) {
        lw_waiter_wakeup(to_wake_up, lock);
    }
    /* Wait. */
    lw_waiter_wait(waiter);
    lw_waiter_clear_src(waiter);
    return;
}

/**
 * Signal the bit cv.
 *
 * @param lock (i/o) the 64-bit word that holds the bits that form the lock.
 * @param lock_mask (i) the bit that represents lock being held.
 * @param wait_mask (i) the bit that is set when waiting.
 * @param cv_mask (i) the bit that is set when doing cond wait.
 */
void
lw_bitlock64_cv_signal(lw_uint64_t *lock,
                       LW_IN lw_uint64_t lock_mask,
                       LW_IN lw_uint64_t wait_mask,
                       LW_IN lw_uint64_t cv_mask)
{
    lw_uint64_t wait_list_idx;
    lw_dlist_t *wait_list;
    lw_waiter_t *to_move = NULL;
    lw_delem_t *elem;
    lw_uint64_t cv_tag = lock_mask | cv_mask;
    lw_uint64_t wait_tag = lock_mask | wait_mask;
    lw_bool_t multiple_waiters = FALSE;
    lw_uint64_t old;

    lw_assert(lock_mask != wait_mask);
    lw_assert(cv_mask != wait_mask);
    lw_assert(lock_mask != cv_mask);
    lw_assert(lock_mask != 0 && wait_mask != 0 && cv_mask != 0);
    lw_assert(LW_IS_POW2(lock_mask) && LW_IS_POW2(wait_mask) && LW_IS_POW2(cv_mask));
    lw_assert((*lock & lock_mask));

    if ((*lock & cv_mask) == 0) {
        /* No one in a cv wait. */
        return;
    }

    wait_list_idx = LW_BITLOCK_PTR_HASH32(lock);
    wait_list_idx = wait_list_idx % wait_lists_count;
    wait_list = &wait_lists[wait_list_idx];

    lw_dl_lock_writer(wait_list);
    elem = wait_list->head;
    while (elem != NULL) {
        lw_waiter_t *waiter = LW_FIELD_2_OBJ_NULL_SAFE(elem, *waiter, event.iface.link);
        if (waiter->event.wait_src == lock && waiter->event.tag == cv_tag) {
            /* Found a waiter. */
            if (to_move == NULL) {
                to_move = waiter;
                to_move->event.tag = wait_tag;
            } else {
                multiple_waiters = TRUE;
                break;
            }
        }
        elem = lw_dl_next(wait_list, elem);
    }
    lw_assert(to_move != NULL);
    lw_assert(to_move->event.tag == wait_tag);
    lw_dl_unlock_writer(wait_list);
    if (!multiple_waiters)  {
        /* Clear cv bit. Lock is held, so no race can take place. */
        old = cv_mask;
        LW_IGNORE_RETURN_VALUE(lw_uint64_swap_with_mask(lock, ~cv_mask, &old, 0));
    }
    /* Set wait bit. */
    old = 0;
    LW_IGNORE_RETURN_VALUE(lw_uint64_swap_with_mask(lock, ~wait_mask, &old, wait_mask));
    return;
}

/**
 * Broadcast the bit cv.
 *
 * @param lock (i/o) the 64-bit word that holds the bits that form the lock.
 * @param lock_mask (i) the bit that represents lock being held.
 * @param wait_mask (i) the bit that is set when waiting.
 * @param cv_mask (i) the bit that is set when doing cond wait.
 */
void
lw_bitlock64_cv_broadcast(lw_uint64_t *lock,
                          LW_IN lw_uint64_t lock_mask,
                          LW_IN lw_uint64_t wait_mask,
                          LW_IN lw_uint64_t cv_mask)
{
    lw_uint64_t wait_list_idx;
    lw_dlist_t *wait_list;
    lw_delem_t *elem;
    lw_uint64_t cv_tag = lock_mask | cv_mask;
    lw_uint64_t wait_tag = lock_mask | wait_mask;
    lw_bool_t atleast_one = FALSE;
    lw_uint64_t old;

    lw_assert(lock_mask != wait_mask);
    lw_assert(cv_mask != wait_mask);
    lw_assert(lock_mask != cv_mask);
    lw_assert(lock_mask != 0 && wait_mask != 0 && cv_mask != 0);
    lw_assert(LW_IS_POW2(lock_mask) && LW_IS_POW2(wait_mask) && LW_IS_POW2(cv_mask));
    lw_assert((*lock & lock_mask));

    if ((*lock & cv_mask) == 0) {
        /* No one in a cv wait. */
        return;
    }

    wait_list_idx = LW_BITLOCK_PTR_HASH32(lock);
    wait_list_idx = wait_list_idx % wait_lists_count;
    wait_list = &wait_lists[wait_list_idx];

    lw_dl_lock_writer(wait_list);
    elem = wait_list->head;
    while (elem != NULL) {
        lw_waiter_t *waiter = LW_FIELD_2_OBJ_NULL_SAFE(elem, *waiter, event.iface.link);
        if (waiter->event.wait_src == lock && waiter->event.tag == cv_tag) {
            /* Found a waiter. */
            atleast_one = TRUE;
            waiter->event.tag = wait_tag;
        }
        elem = lw_dl_next(wait_list, elem);
    }
    lw_assert(atleast_one);
    LW_UNUSED_PARAMETER(atleast_one);
    lw_dl_unlock_writer(wait_list);
    /* Clear cv bit. Lock is held, so no race can take place. Also set wait bit. */
    old = cv_mask;
    LW_IGNORE_RETURN_VALUE(lw_uint64_swap_with_mask(lock, ~cv_mask, &old, 0));
    old = 0;
    LW_IGNORE_RETURN_VALUE(lw_uint64_swap_with_mask(lock, ~wait_mask, 0, old));
    return;
}

/**
 * lw_bitlock64_rekey -- move all waiters related to this lock to the new lock.
 * Caller must have the locks held and is responsible for ensuring that no new
 * waiters arrive for the old lock once rekey is started.
 *
 * @param lock (i) the 64-bit word that holds the bits that form the lock.
 * @param lock (i) the 64-bit word that is the new lock pointer.
 * @param lock_mask (i) the bit that represents lock being held.
 * @param wait_mask (i) the bit that is set when waiting.
 * @param cv_mask (i) the bit that is set when doing cond wait.
 */
void
lw_bitlock64_rekey(LW_INOUT lw_uint64_t *lock,
                   LW_INOUT lw_uint64_t *newlock,
                   LW_IN lw_uint64_t lock_mask,
                   LW_IN lw_uint64_t wait_mask,
                   LW_IN lw_uint64_t cv_mask)
{
    lw_uint64_t wait_list_idx;
    lw_dlist_t *wait_list;
    lw_dlist_t waiters_to_move;
    lw_delem_t *elem;
    lw_uint64_t cv_tag = lock_mask | cv_mask;
    lw_uint64_t wait_tag = lock_mask | wait_mask;
    lw_uint64_t old;
    lw_bool_t have_cv_waiter = FALSE;
    lw_bool_t have_lock_waiter = FALSE;

    lw_assert(lock_mask != wait_mask);
    lw_assert(cv_mask != wait_mask);
    lw_assert(lock_mask != cv_mask);
    lw_assert(lock_mask != 0 && wait_mask != 0 && cv_mask != 0);
    lw_assert(LW_IS_POW2(lock_mask) && LW_IS_POW2(wait_mask) && LW_IS_POW2(cv_mask));
    lw_assert((*lock & lock_mask));
    lw_assert((*newlock & lock_mask));

    old = *lock;
    if (!((old & wait_mask) == 1 || (old & cv_mask) == 1)) {
        /* Nothing to move. */
        return;
    }

    lw_dl_init(&waiters_to_move);
    wait_list_idx = LW_BITLOCK_PTR_HASH32(lock);
    wait_list_idx = wait_list_idx % wait_lists_count;
    wait_list = &wait_lists[wait_list_idx];

    lw_dl_lock_writer(wait_list);
    elem = wait_list->head;
    while (elem != NULL) {
        lw_waiter_t *waiter = LW_FIELD_2_OBJ_NULL_SAFE(elem, *waiter, event.iface.link);
        lw_delem_t *next = lw_dl_next(wait_list, elem);
        if (waiter->event.wait_src == lock &&
            (waiter->event.tag == cv_tag || waiter->event.tag == wait_tag)) {
            /* Found a waiter. */
            lw_dl_remove(wait_list, elem);
            lw_dl_append_at_end(&waiters_to_move, elem);
            waiter->event.wait_src = newlock;
            if (!have_lock_waiter) {
                have_lock_waiter = waiter->event.tag == wait_tag;
            }
            if (!have_cv_waiter) {
                have_cv_waiter = waiter->event.tag == cv_tag;
            }
        }
        elem = next;
    }
    lw_dl_unlock_writer(wait_list);

    wait_list_idx = LW_BITLOCK_PTR_HASH32(newlock);
    wait_list_idx = wait_list_idx % wait_lists_count;
    wait_list = &wait_lists[wait_list_idx];

    lw_dl_lock_writer(wait_list);
    lw_verify(lw_dl_get_count(&waiters_to_move) > 0);
    elem = waiters_to_move.head;
    while (elem != NULL) {
        lw_delem_t *next = lw_dl_next(&waiters_to_move, elem);
        lw_dl_remove(&waiters_to_move, elem);
        lw_dl_append_at_end(wait_list, elem);
        elem = next;
    }
    lw_dl_destroy(&waiters_to_move);
    /* Set appropirate wait/cv bits on new lock and clear from old lock. */
    if (have_lock_waiter) {
        old = 0;
        LW_IGNORE_RETURN_VALUE(lw_uint64_swap_with_mask(newlock, ~wait_mask, &old, wait_mask));
        old = wait_mask;
        LW_IGNORE_RETURN_VALUE(lw_uint64_swap_with_mask(lock, ~wait_mask, &old, 0));
    }
    if (have_cv_waiter) {
        old = 0;
        LW_IGNORE_RETURN_VALUE(lw_uint64_swap_with_mask(newlock, ~cv_mask, &old, cv_mask));
        old = cv_mask;
        LW_IGNORE_RETURN_VALUE(lw_uint64_swap_with_mask(lock, ~cv_mask, &old, 0));
    }
    return;
}
