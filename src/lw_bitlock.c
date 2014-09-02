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
 * or not (FALSE, needs to call lw_bitlock_complete_wait later).
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
 * Release a bitlock. If there are waiters, the lock is handed over to the 1st one.
 *
 * @param lock (i/o) the 32-bit word that holds the bits that form the lock.
 * @param lock_mask (i) the bit that represents lock being held.
 * @param wait_mask (i) the bit that is set when waiting.
 * @returns the pointer to the waiter of the new owner. Caller has to signal the waiter to wake up.
 */
lw_waiter_t *
lw_bitlock32_unlock_return_waiter(lw_uint32_t *lock,
                                  LW_IN lw_uint32_t lock_mask,
                                  LW_IN lw_uint32_t wait_mask)
{
    lw_uint32_t wait_list_idx;
    lw_dlist_t *wait_list;
    lw_waiter_t *to_wake_up = NULL;
    lw_waiter_t *curr_waiter, *temp_waiter = NULL;

    lw_assert(lock_mask != wait_mask);
    lw_assert(lock_mask != 0 && wait_mask != 0);
    lw_assert(LW_IS_POW2(lock_mask) && LW_IS_POW2(wait_mask));
    lw_assert(*lock & lock_mask);

    if (lw_bitlock32_drop_lock_if_no_waiters(lock, lock_mask, wait_mask)) {
        /* All done. */
        return NULL;
    }

    wait_list_idx = LW_BITLOCK_PTR_HASH32(lock);
    wait_list_idx = wait_list_idx % wait_lists_count;
    wait_list = &wait_lists[wait_list_idx];

    curr_waiter = lw_waiter_get();
    if (curr_waiter->event.wait_src != NULL) {
        temp_waiter = lw_waiter_alloc();
        curr_waiter->domain->set_waiter(curr_waiter->domain, temp_waiter);
    }
    lw_dl_lock_writer(wait_list);
    to_wake_up = lw_bitlock32_get_one_waiter(wait_list, lock, lock_mask, wait_mask);
    lw_assert(to_wake_up != NULL);
    lw_dl_unlock_writer(wait_list);
    if (temp_waiter != NULL) {
        curr_waiter->domain->set_waiter(curr_waiter->domain, curr_waiter);
        lw_waiter_free(temp_waiter);
    }
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
 * or not (FALSE, needs to call lw_bitlock_complete_wait later).
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
                     LW_IN lw_uint32_t cv_mask,
                     LW_IN lw_bool_t sync)
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
    if (sync) {
        /* Wait. */
        lw_waiter_wait(waiter);
        lw_waiter_clear_src(waiter);
    }
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
    LW_IGNORE_RETURN_VALUE(lw_uint32_swap_with_mask(lock, ~wait_mask, &old, wait_mask));
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
 * Release a bitlock. If there are waiters, the lock is handed over to the 1st one.
 *
 * @param lock (i/o) the 64-bit word that holds the bits that form the lock.
 * @param lock_mask (i) the bit that represents lock being held.
 * @param wait_mask (i) the bit that is set when waiting.
 * @returns the pointer to the waiter of the new owner. Caller has to signal the waiter to wake up.
 */
lw_waiter_t *
lw_bitlock64_unlock_return_waiter(lw_uint64_t *lock,
                                  LW_IN lw_uint64_t lock_mask,
                                  LW_IN lw_uint64_t wait_mask)
{
    lw_uint64_t wait_list_idx;
    lw_dlist_t *wait_list;
    lw_waiter_t *to_wake_up = NULL;
    lw_waiter_t *curr_waiter, *temp_waiter = NULL;

    lw_assert(lock_mask != wait_mask);
    lw_assert(lock_mask != 0 && wait_mask != 0);
    lw_assert(LW_IS_POW2(lock_mask) && LW_IS_POW2(wait_mask));
    lw_assert(*lock & lock_mask);

    if (lw_bitlock64_drop_lock_if_no_waiters(lock, lock_mask, wait_mask)) {
        /* All done. */
        return NULL;
    }

    wait_list_idx = LW_BITLOCK_PTR_HASH32(lock);
    wait_list_idx = wait_list_idx % wait_lists_count;
    wait_list = &wait_lists[wait_list_idx];

    curr_waiter = lw_waiter_get();
    if (curr_waiter->event.wait_src != NULL) {
        temp_waiter = lw_waiter_alloc();
        curr_waiter->domain->set_waiter(curr_waiter->domain, temp_waiter);
    }
    lw_dl_lock_writer(wait_list);
    to_wake_up = lw_bitlock64_get_one_waiter(wait_list, lock, lock_mask, wait_mask);
    lw_assert(to_wake_up != NULL);
    lw_dl_unlock_writer(wait_list);
    if (temp_waiter != NULL) {
        curr_waiter->domain->set_waiter(curr_waiter->domain, curr_waiter);
        lw_waiter_free(temp_waiter);
    }
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
                     LW_IN lw_uint64_t cv_mask,
                     LW_IN lw_bool_t sync)
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
    if (sync) {
        /* Wait. */
        lw_waiter_wait(waiter);
        lw_waiter_clear_src(waiter);
    }
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
    LW_IGNORE_RETURN_VALUE(lw_uint64_swap_with_mask(lock, ~wait_mask, &old, wait_mask));
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
    if ((old & wait_mask) == 0 && (old & cv_mask) == 0) {
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
            lw_waiter_change_src(waiter, lock, newlock);
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

    lw_verify(lw_dl_get_count(&waiters_to_move) > 0);
    lw_dl_lock_writer(wait_list);
    elem = waiters_to_move.head;
    while (elem != NULL) {
        lw_delem_t *next = lw_dl_next(&waiters_to_move, elem);
        lw_dl_remove(&waiters_to_move, elem);
        lw_dl_append_at_end(wait_list, elem);
        elem = next;
    }
    lw_dl_unlock_writer(wait_list);
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
    lw_assert(!(*lock & wait_mask));
    lw_assert(!(*lock & cv_mask));
    return;
}

#define RW_UNIQ_BITS        (3)

typedef union {
    lw_uint64_t word64;
    struct {
        lw_uint64_t  start_bit:6;
        lw_uint64_t  num_bits:6;
        lw_uint64_t  rdlock:1;
        lw_uint64_t  in_list:1;
        lw_uint64_t  have_lock:1;
        lw_uint64_t  need_to_wait:1;
        lw_uint64_t  waited_for_lock:1;
        lw_uint64_t  rw_uniq:RW_UNIQ_BITS;   // Ensures no collision with tags used for mutex use cases.
        lw_uint64_t  unused:44;
    } fields;
} bitlock_rw_wait_t;

LW_STATIC_ASSERT(sizeof(bitlock_rw_wait_t) == sizeof(lw_uint64_t), bitlock_wait_missized);

static lw_uint32_t
lw_bitlock32_set_rdlock(lw_uint32_t *lock,
                        lw_uint32_t read_increment,
                        lw_uint32_t read_mask,
                        lw_uint32_t write_mask,
                        lw_bool_t try,
                        lw_bool_t under_wait_list_lock)
{
    lw_uint32_t old, new;

    lw_assert((read_increment & (read_increment - 1)) == 0);
    lw_assert((write_mask & (write_mask - 1)) == 0);
    lw_assert((read_mask & write_mask) == 0);
    lw_assert(read_increment == (write_mask << 1));
    lw_assert((read_mask & read_increment) == read_increment);

    old = *lock;
    do {
        if ((old & write_mask) != 0) {
            /* Writer bit is set. Writer waiting or already running. */
            if (!under_wait_list_lock || try) {
                /* Need to do this under wait list lock. */
                return old;
            }
            if ((old & read_mask) != 0) {
                /* Reader count already non-zero. Nothing more to do. */
                return old;
            }
            new = old + read_increment; // Set reader count to 1 to indicate contention to writer.
        } else {
            if ((old & read_mask) == read_mask) {
                /* Reader count already saturated. */
                return old;
            }
            new = old + read_increment; // Increment reader count.
        }
    } while (!lw_uint32_swap(lock, &old, new));

    return old;
}

static lw_uint32_t
lw_bitlock32_clear_rdlock(lw_uint32_t *lock,
                          lw_uint32_t read_increment,
                          lw_uint32_t read_mask,
                          lw_uint32_t write_mask)
{
    lw_uint32_t old, new;

    lw_assert((read_increment & (read_increment - 1)) == 0);
    lw_assert((write_mask & (write_mask - 1)) == 0);
    lw_assert((read_mask & write_mask) == 0);
    lw_assert(read_increment == (write_mask << 1));
    lw_assert((read_mask & read_increment) == read_increment);

    old = *lock;
    do {
        lw_assert((old & read_mask) != 0);
        if ((old & read_mask) == read_mask) {
            /* Read count is saturated. Caller does custom update. */
            return old;
        } else if ((old & read_mask) == read_increment &&
                   (old & write_mask) != 0) {
            /* Writer bit is set. Writer is waiting. Caller to do custom work under lock. */
            return old;
        }
        new = old - read_increment;
    } while (!lw_uint32_swap(lock, &old, new));

    return old;
}

static lw_uint32_t
lw_bitlock32_set_wrlock(lw_uint32_t *lock,
                        lw_uint32_t read_increment,
                        lw_uint32_t read_mask,
                        lw_uint32_t write_mask,
                        lw_bool_t try,
                        lw_bool_t under_wait_list_lock)
{
    lw_uint32_t old, new;

    lw_assert((read_increment & (read_increment - 1)) == 0);
    lw_assert((write_mask & (write_mask - 1)) == 0);
    lw_assert((read_mask & write_mask) == 0);
    lw_assert(read_increment == (write_mask << 1));
    lw_assert((read_mask & read_increment) == read_increment);

    old = *lock;
    do {
        if ((old & write_mask) != 0) {
            /* Writer bit is set. This caller needs to wait. */
            if (!under_wait_list_lock || try) {
                /* Need to do this under wait list lock. */
                return old;
            }
            if ((old & read_mask) != 0) {
                /* Reader count already non-zero. Nothing more to do. */
                return old;
            }
            new = old + read_increment; // Set reader count to 1 to indicate contention to writer.
            lw_assert(new == (write_mask | read_increment));
        } else if (old & read_mask) {
            if (!under_wait_list_lock || try) {
                return old; // Caller has to do this under lock.
            }
            new = old | write_mask;
        } else {
            new = old | write_mask;
        }
    } while (!lw_uint32_swap(lock, &old, new));

    return old;
}

static lw_uint32_t
lw_bitlock32_clear_wrlock(lw_uint32_t *lock,
                          lw_uint32_t read_increment,
                          lw_uint32_t read_mask,
                          lw_uint32_t write_mask,
                          lw_bool_t under_wait_list_lock)
{
    lw_uint32_t old, new;

    lw_assert((read_increment & (read_increment - 1)) == 0);
    lw_assert((write_mask & (write_mask - 1)) == 0);
    lw_assert((read_mask & write_mask) == 0);
    lw_assert(read_increment == (write_mask << 1));
    lw_assert((read_mask & read_increment) == read_increment);
    lw_assert(!under_wait_list_lock); // Not called when holding lock.

    old = *lock;
    do {
        lw_assert((old & write_mask) == write_mask);
        if ((old & read_mask) != 0) {
            /* Reader count != 0 indicates contention. */
            if (!under_wait_list_lock) {
                /* Not called under wait list lock, so can't do anything. */
                return old;
            }
            lw_verify(FALSE); // Caller should compute new state directly.
        }
        new = old & (~write_mask);
    } while (!lw_uint32_swap(lock, &old, new));

    return old;
}

void
lw_bitlock_rw_complete_wait(lw_uint8_t *waiter_buf)
{
    bitlock_rw_wait_t wait_tag;
    lw_waiter_t *waiter = (lw_waiter_t *)waiter_buf;

    wait_tag.word64 = waiter->event.tag;
    lw_verify(wait_tag.fields.waited_for_lock == 0 &&
              wait_tag.fields.need_to_wait == 1);
    lw_waiter_wait(waiter);
    wait_tag.word64 = waiter->event.tag;
    lw_verify(wait_tag.fields.have_lock);
    wait_tag.fields.waited_for_lock = 1;
    waiter->event.tag = wait_tag.word64;
    lw_waiter_clear_src(waiter);
    lw_waiter_assert_src(waiter, NULL);
}

/**
 * Init a rw bitlock. Clears the writer and readers bits.
 *
 * @param lock (i/o) the 32-bit word to init as a lock.
 * @param start_bit (i) the bit at which the lock starts.
 * @param num_bits (i) the number of bits in the lock.
 */
void
lw_bitlock32_rwlock_init(lw_uint32_t *lock,
                         LW_IN lw_uint32_t start_bit,
                         LW_IN lw_uint32_t num_bits)
{

    lw_uint32_t writer_mask = 1 << start_bit;
    lw_uint32_t reader_mask = ((1 << (num_bits - 1)) - 1) << (start_bit + 1);

    lw_assert(start_bit < 30); // At least 3 bits required for lock.
    lw_assert(num_bits > 2); // At least 3 bits required for lock.
    lw_assert(num_bits < 32); // No point using all bits. Might as well use lw_rwlock.

    *lock = *lock & ~(reader_mask | writer_mask);
}

void
lw_bitlock32_rwlock_destroy(lw_uint32_t *lock,
                            LW_IN lw_uint32_t start_bit,
                            LW_IN lw_uint32_t num_bits)
{

    lw_uint32_t writer_mask = 1 << start_bit;
    lw_uint32_t reader_mask = ((1 << (num_bits - 1)) - 1) << (start_bit + 1);

    lw_assert(start_bit < 30); // At least 3 bits required for lock.
    lw_assert(num_bits > 2); // At least 3 bits required for lock.
    lw_assert(num_bits < 32); // No point using all bits. Might as well use lw_rwlock.

    lw_verify((*lock & (reader_mask | writer_mask)) == 0);
}

/*
 * Take a read lock on a rwlock made out of bits from a 32 bit word. The bit-range to use for
 * locking is specified. The waiter_buf is used in case of overflow accounting of reader bits.
 * The waiter_buf is expected to come out of the stack of the caller in most cases.
 *
 * Return TRUE if lock is immediately acquired. FALSE otherwise.
 */
lw_bool_t
lw_bitlock32_readlock_async(lw_uint32_t *lock,
                            LW_IN lw_uint32_t start_bit,
                            LW_IN lw_uint32_t num_bits,
                            LW_IN lw_bool_t try_lock,
                            lw_uint8_t *waiter_buf)
{
    lw_uint32_t old;
    lw_waiter_t *waiter = (lw_waiter_t *)waiter_buf;
    bitlock_rw_wait_t wait_tag;
    lw_uint32_t wait_list_idx;
    lw_dlist_t *wait_list;
    lw_waiter_domain_t *domain;

    lw_uint32_t writer_mask = 1 << start_bit;
    lw_uint32_t reader_mask = ((1 << (num_bits - 1)) - 1) << (start_bit + 1);
    lw_uint32_t reader_inc = 1 << (start_bit + 1); // What incrementing the read count effectively adds.

    lw_assert(start_bit < 30); // At least 3 bits required for lock.
    lw_assert(num_bits > 2); // At least 3 bits required for lock.
    lw_assert(num_bits < 32); // No point using all bits. Might as well use lw_rwlock.
    lw_assert(waiter != lw_waiter_get()); // Cannot use thread's waiter as code messes with it.

    wait_tag.word64 = 0;
    wait_tag.fields.start_bit = start_bit;
    wait_tag.fields.num_bits = num_bits;
    wait_tag.fields.rdlock = 1;
    old = lw_bitlock32_set_rdlock(lock, reader_inc, reader_mask, writer_mask, try_lock, FALSE);
    if ((old & reader_mask) != reader_mask && (old & writer_mask) == 0) {
        /* Got lock. */
        wait_tag.fields.have_lock = 1;
        lw_assert(wait_tag.fields.in_list == 0);
        waiter->event.tag = wait_tag.word64;
        return TRUE;
    }

    if (try_lock && (old & writer_mask) != 0) {
        return FALSE;
    }

    wait_list_idx = LW_BITLOCK_PTR_HASH32(lock);
    wait_list_idx = wait_list_idx % wait_lists_count;
    wait_list = &wait_lists[wait_list_idx];
    lw_dl_lock_writer(wait_list);
    old = lw_bitlock32_set_rdlock(lock, reader_inc, reader_mask, writer_mask, try_lock, TRUE);
    if ((old & reader_mask) != reader_mask && (old & writer_mask) == 0) {
        /* Got lock. */
        lw_assert(wait_tag.fields.in_list == 0);
        wait_tag.fields.have_lock = 1;
        waiter->event.tag = wait_tag.word64;
        lw_dl_unlock_writer(wait_list);
        return TRUE;
    }
    if (try_lock && (old & writer_mask) != 0) {
        lw_dl_unlock_writer(wait_list);
        return FALSE;
    }
    wait_tag.fields.start_bit = start_bit;
    wait_tag.fields.num_bits = num_bits;
    wait_tag.fields.rw_uniq = ~0;    // All the 1s ensure no false match with waiters from bitlock mutexes.
    wait_tag.fields.unused = 0;
    wait_tag.fields.in_list = 1;
    wait_tag.fields.have_lock = ((old & writer_mask) == 0);
    wait_tag.fields.need_to_wait = !wait_tag.fields.have_lock;
    domain = lw_waiter_global_domain;
    domain->waiter_event_init(domain, waiter);
    lw_waiter_set_src(waiter, lock);
    waiter->event.tag = wait_tag.word64;
    lw_dl_append_at_end(wait_list, &waiter->event.iface.link);
    lw_dl_unlock_writer(wait_list);
    return wait_tag.fields.have_lock;
}

lw_int32_t
lw_bitlock32_try_readlock(lw_uint32_t *lock,
                          LW_IN lw_uint32_t start_bit,
                          LW_IN lw_uint32_t num_bits,
                          lw_uint8_t *waiter_buf)
{
    if (lw_bitlock32_readlock_async(lock, start_bit, num_bits, TRUE, waiter_buf)) {
        return 0;
    }
    return EBUSY;
}

static lw_waiter_t *
find_next_matching_waiter(lw_dlist_t *list, lw_delem_t *last, lw_uint64_t tag)
{
    bitlock_rw_wait_t wait_tag;
    lw_delem_t *elem;

    wait_tag.word64 = tag;

    if (last == NULL) {
        elem = list->head;
    } else {
        elem = lw_dl_next(list, last);
    }

    while (elem != NULL) {
        lw_waiter_t *waiter = LW_FIELD_2_OBJ(elem, *waiter, event.iface.link);
        bitlock_rw_wait_t waiter_tag;

        waiter_tag.word64 = waiter->event.tag;
        if (waiter_tag.fields.rw_uniq == ((1 << RW_UNIQ_BITS) - 1) &&
            waiter_tag.fields.start_bit == wait_tag.fields.start_bit &&
            waiter_tag.fields.num_bits == wait_tag.fields.num_bits) {
            lw_assert(waiter_tag.fields.in_list == 1);
            return waiter;
        }
        elem = lw_dl_next(list, elem);
    }
    return NULL;
}

/* Drop a bit range based read lock. The waiter_buf should be the same as used in the lock call. */
void
lw_bitlock32_readunlock(lw_uint32_t *lock,
                        LW_IN lw_uint32_t start_bit,
                        LW_IN lw_uint32_t num_bits,
                        lw_uint8_t *waiter_buf)
{
    lw_uint32_t old, new;
    lw_waiter_t *waiter = (lw_waiter_t *)waiter_buf;
    lw_waiter_t *first_match = NULL;
    bitlock_rw_wait_t wait_tag;
    lw_uint32_t wait_list_idx;
    lw_dlist_t *wait_list;
    lw_waiter_domain_t *domain = lw_waiter_global_domain;
    lw_bool_t do_wakeup = FALSE;

    lw_uint32_t writer_mask = 1 << start_bit;
    lw_uint32_t reader_mask = ((1 << (num_bits - 1)) - 1) << (start_bit + 1);
    lw_uint32_t reader_inc = 1 << (start_bit + 1); // What incrementing the read count effectively adds.

    lw_assert(start_bit < 30); // At least 3 bits required for lock.
    lw_assert(num_bits > 2); // At least 3 bits required for lock.
    lw_assert(num_bits < 32); // No point using all bits. Might as well use lw_rwlock.
    lw_assert(waiter != lw_waiter_get()); // Cannot use thread's waiter as code messes with it.

    wait_tag.word64 = waiter->event.tag;
    lw_assert(wait_tag.fields.have_lock == 1);
    lw_assert((*lock & reader_mask) != 0);
    old = lw_bitlock32_clear_rdlock(lock, reader_inc, reader_mask, writer_mask);
    if (!((old & reader_mask) == reader_mask ||
          ((old & writer_mask) != 0 && (old & reader_mask) == reader_inc))) {
        /* Common case: lock dropped and there was no contending waiter or other readers around. */
        wait_tag.word64 = waiter->event.tag;
        lw_assert(wait_tag.fields.in_list == 0);
        if (wait_tag.fields.need_to_wait) {
            domain->waiter_event_destroy(domain, waiter);
        }
        return;
    }

    wait_tag.fields.start_bit = start_bit;
    wait_tag.fields.num_bits = num_bits;
    wait_list_idx = LW_BITLOCK_PTR_HASH32(lock);
    wait_list_idx = wait_list_idx % wait_lists_count;
    wait_list = &wait_lists[wait_list_idx];

    lw_dl_lock_writer(wait_list);

    old = lw_bitlock32_clear_rdlock(lock, reader_inc, reader_mask, writer_mask);
    if (!((old & reader_mask) == reader_mask ||
          ((old & writer_mask) != 0 && (old & reader_mask) == reader_inc))) {
        /* Common case: lock dropped and there was no contending waiter or other readers around. */
        wait_tag.word64 = waiter->event.tag;
        lw_verify(wait_tag.fields.in_list == 0);
        lw_dl_unlock_writer(wait_list);
        if (wait_tag.fields.need_to_wait) {
            domain->waiter_event_destroy(domain, waiter);
        }
        return;
    }

    /*
     * Read unlock has to come down this path only if the reader bits were saturated or if the
     * current reader is the last one and there is a writer waiting. In either case, once the
     * list is locked, no change can happen to the lock. Any new reader will have to take the
     * lock and writer too of course. So we can just calculate what needs to be done without
     * worrying about concurrent threads.
     */
    old = *lock;
    new = old;
    lw_assert((old & reader_mask) == reader_mask ||
              ((old & writer_mask) != 0 && (old & reader_mask) == reader_inc));
    wait_tag.word64 = waiter->event.tag;
    if (wait_tag.fields.in_list == 1) {
        lw_dl_remove(wait_list, &waiter->event.iface.link);
        lw_verify((old & reader_mask) == reader_mask);
        wait_tag.fields.in_list = 0;
        waiter->event.tag = wait_tag.word64;
        first_match = waiter;
    } else {
        wait_tag.fields.start_bit = start_bit;
        wait_tag.fields.num_bits = num_bits;
        first_match = find_next_matching_waiter(wait_list, NULL, wait_tag.word64);
        if (first_match != NULL) {
            wait_tag.word64 = first_match->event.tag;
            if (wait_tag.fields.have_lock) {
                /*
                 * There was overflow in reader count bits. Remove the reader's waiter and leave the
                 * rest alone.
                 */
                lw_assert(wait_tag.fields.rdlock == 1);
                lw_assert((old & reader_mask) == reader_mask);
                lw_dl_remove(wait_list, &first_match->event.iface.link);
                wait_tag.fields.in_list = 0;
                first_match->event.tag = wait_tag.word64;
            } else {
                lw_waiter_t *next_waiter;
                lw_verify(!wait_tag.fields.rdlock); // Waiting writer.
                next_waiter = find_next_matching_waiter(wait_list,
                                                        &first_match->event.iface.link,
                                                        wait_tag.word64);
                do_wakeup = ((old & reader_mask) == reader_inc);
                if (do_wakeup) {
                    lw_dl_remove(wait_list, &first_match->event.iface.link);
                    wait_tag.fields.in_list = 0;
                    first_match->event.tag = wait_tag.word64;
                }
                if (next_waiter == NULL || (old & reader_mask) == reader_mask) {
                    new = new - reader_inc;
                }
            }
        } else {
            new = new - reader_inc;
        }
    }

    lw_assert(!do_wakeup || (old & reader_mask) == reader_inc);

    if (old != new) {
        lw_bool_t swapped;
        lw_uint32_t new = old - reader_inc;
        lw_assert((old & reader_mask) == reader_mask ||
                  ((old & writer_mask) != 0 && (old & reader_mask) == reader_inc));
        swapped = lw_uint32_swap_with_mask(lock, ~reader_mask, &old, new);
        lw_verify(swapped);
    } else {
        lw_assert(((old & reader_mask) == reader_mask &&
                   ((bitlock_rw_wait_t)first_match->event.tag).fields.have_lock &&
                   ((bitlock_rw_wait_t)first_match->event.tag).fields.rdlock) ||
                  ((old & writer_mask) != 0 && (old & reader_mask) != 0));
    }

    lw_dl_unlock_writer(wait_list);
    if (do_wakeup) {
        wait_tag.word64 = first_match->event.tag;
        lw_assert(wait_tag.fields.rdlock == 0 && wait_tag.fields.in_list == 0);
        wait_tag.fields.have_lock = 1;
        first_match->event.tag = wait_tag.word64;
        lw_waiter_wakeup(first_match, lock);
    }
    wait_tag.word64 = waiter->event.tag;
    if (wait_tag.fields.need_to_wait) {
        domain->waiter_event_destroy(domain, waiter);
    }
}

/* A generic function to execute while holding a read lock. */
void
with_bitlock_reader(lw_uint32_t *lock,
                    LW_IN lw_uint32_t start_bit,
                    LW_IN lw_uint32_t num_bits,
                    callback_func_t callback,
                    void *arg)
{
    lw_waiter_domain_t *domain = lw_waiter_global_domain;
    lw_uint8_t  waiter_buf[domain->waiter_size];

    lw_bitlock32_readlock(lock, start_bit, num_bits, waiter_buf);
    callback(arg);
    lw_bitlock32_readunlock(lock, start_bit, num_bits, waiter_buf);
}

/*
 * Take a write lock on a rwlock made out of bits from a 32 bit word. The bit-range to use for locking
 * is specified. The waiter_buf is used in case the caller has to wait or for overflow accounting of
 * writeer bits. The waiter_buf is expected to come out of the stack of the caller in most cases.
 *
 * Return TRUE if lock is immediately acquired. FALSE otherwise.
 */
lw_bool_t
lw_bitlock32_writelock_async(lw_uint32_t *lock,
                             LW_IN lw_uint32_t start_bit,
                             LW_IN lw_uint32_t num_bits,
                             LW_IN lw_bool_t try_lock)
{
    lw_uint32_t old;
    lw_waiter_t *waiter = lw_waiter_get();
    bitlock_rw_wait_t wait_tag;
    lw_uint32_t wait_list_idx;
    lw_dlist_t *wait_list;

    lw_uint32_t writer_mask = 1 << start_bit;
    lw_uint32_t reader_mask = ((1 << (num_bits - 1)) - 1) << (start_bit + 1);
    lw_uint32_t reader_inc = 1 << (start_bit + 1); // What incrementing the write count effectively adds.

    lw_assert(start_bit < 30); // At least 3 bits required for lock.
    lw_assert(num_bits > 2); // At least 3 bits required for lock.
    lw_assert(num_bits < 32); // No point using all bits. Might as well use lw_rwlock.

    wait_tag.word64 = 0;
    wait_tag.fields.start_bit = start_bit;
    wait_tag.fields.num_bits = num_bits;
    old = lw_bitlock32_set_wrlock(lock, reader_inc, reader_mask, writer_mask, try_lock, FALSE);
    if ((old & reader_mask) == 0 && (old & writer_mask) == 0) {
        /* Got lock. */
        wait_tag.fields.have_lock = 1;
        waiter->event.tag = wait_tag.word64;
        return TRUE;
    }

    if (try_lock) {
        return FALSE;
    }

    wait_list_idx = LW_BITLOCK_PTR_HASH32(lock);
    wait_list_idx = wait_list_idx % wait_lists_count;
    wait_list = &wait_lists[wait_list_idx];
    lw_dl_lock_writer(wait_list);
    old = lw_bitlock32_set_wrlock(lock, reader_inc, reader_mask, writer_mask, try_lock, TRUE);
    if ((old & reader_mask) == 0 && (old & writer_mask) == 0) {
        /* Got lock. */
        wait_tag.fields.have_lock = 1;
        waiter->event.tag = wait_tag.word64;
        lw_dl_unlock_writer(wait_list);
        return TRUE;
    }
    wait_tag.fields.start_bit = start_bit;
    wait_tag.fields.num_bits = num_bits;
    wait_tag.fields.unused = 0;
    wait_tag.fields.rw_uniq = ~0;    // All the 1s ensure no false match with waiters from bitlock mutexes.
    wait_tag.fields.in_list = 1;
    wait_tag.fields.have_lock = 0;
    wait_tag.fields.need_to_wait = 1;
    lw_waiter_set_src(waiter, lock);
    waiter->event.tag = wait_tag.word64;
    lw_dl_append_at_end(wait_list, &waiter->event.iface.link);
    lw_dl_unlock_writer(wait_list);
    return FALSE;
}

lw_int32_t
lw_bitlock32_try_writelock(lw_uint32_t *lock,
                          LW_IN lw_uint32_t start_bit,
                          LW_IN lw_uint32_t num_bits)
{
    if (lw_bitlock32_writelock_async(lock, start_bit, num_bits, TRUE)) {
        return 0;
    }
    return EBUSY;
}

/* Find all waiters to wake up on writer lock release. Return whether more remain after. */
static lw_bool_t
find_eligible_waiters(lw_dlist_t *list,
                      lw_dlist_t *to_wakeup,
                      lw_uint64_t tag,
                      lw_uint32_t max_to_dequeue,
                      lw_bool_t *next_is_reader)
{
    lw_waiter_t *match;
    lw_bool_t want_readers = TRUE;
    bitlock_rw_wait_t wait_tag;

    match = find_next_matching_waiter(list, NULL, tag);

    if (match == NULL) {
        return FALSE;
    } else {
        wait_tag.word64 = match->event.tag;
        want_readers = (wait_tag.fields.rdlock == 1);
        if (!want_readers) {
            max_to_dequeue = 1;
        }
    }

    while (match != NULL) {
        lw_waiter_t *next = find_next_matching_waiter(list, &match->event.iface.link, tag);

        wait_tag.word64 = match->event.tag;
        if (want_readers == (wait_tag.fields.rdlock == 1) &&
            lw_dl_get_count(to_wakeup) < max_to_dequeue) {
            lw_dl_remove(list, &match->event.iface.link);
            lw_dl_append_at_end(to_wakeup, &match->event.iface.link);
        } else {
            /* found too many waiters or a waiter that wants a lock of different type. */
            *next_is_reader = wait_tag.fields.rdlock;
            return TRUE;
        }
        match = next;
    }
    return FALSE;
}

/**
 * Wake up all contiguous readers in the given list for the given tag.
 * @returns TRUE if a writer is encountered. FALSE otherwise.
 */
static lw_bool_t
wake_eligible_readers(lw_dlist_t *list,
                      lw_uint64_t tag,
                      void *lock,
                      lw_bool_t remove_from_list)
{
    lw_waiter_t *match;
    bitlock_rw_wait_t wait_tag;

    match = find_next_matching_waiter(list, NULL, tag);

    while (match != NULL) {
        lw_waiter_t *next = find_next_matching_waiter(list, &match->event.iface.link, tag);

        wait_tag.word64 = match->event.tag;
        if (wait_tag.fields.rdlock) {
            if (remove_from_list) {
                lw_dl_remove(list, &match->event.iface.link);
                wait_tag.fields.in_list = 0;
            }
            wait_tag.fields.have_lock = 1;
            match->event.tag = wait_tag.word64;
            lw_waiter_wakeup(match, lock);
        } else {
            /* found a waiter that wants a lock of different type. */
            return TRUE;
        }
        match = next;
    }
    return FALSE;
}

/* Drop a bit range based write lock. The waiter_buf should be the same as used in the lock call. */
void
lw_bitlock32_writeunlock(lw_uint32_t *lock,
                         LW_IN lw_uint32_t start_bit,
                         LW_IN lw_uint32_t num_bits)
{
    lw_uint32_t old, new;
    bitlock_rw_wait_t wait_tag;
    lw_uint32_t wait_list_idx;
    lw_dlist_t *wait_list;
    lw_bool_t have_more = TRUE;
    lw_dlist_t to_wakeup;
    lw_waiter_t *first;
    lw_bool_t next_is_reader = FALSE;

    lw_uint32_t writer_mask = 1 << start_bit;
    lw_uint32_t max_readers = ((1 << (num_bits - 1)) - 1);
    lw_uint32_t reader_mask = max_readers << (start_bit + 1);
    lw_uint32_t reader_inc = 1 << (start_bit + 1); // What incrementing the write count effectively adds.

    lw_assert(start_bit < 30); // At least 3 bits required for lock.
    lw_assert(num_bits > 2); // At least 3 bits required for lock.
    lw_assert(num_bits < 32); // No point using all bits. Might as well use lw_rwlock.

    old = lw_bitlock32_clear_wrlock(lock, reader_inc, reader_mask, writer_mask, FALSE);
    if ((old & reader_mask) == 0) {
        /* Common case: lock dropped and there was no contending waiter. */
        return;
    }

    wait_tag.fields.start_bit = start_bit;
    wait_tag.fields.num_bits = num_bits;
    lw_dl_init(&to_wakeup);
    wait_list_idx = LW_BITLOCK_PTR_HASH32(lock);
    wait_list_idx = wait_list_idx % wait_lists_count;
    wait_list = &wait_lists[wait_list_idx];
    lw_dl_lock_writer(wait_list);
    /*
     * While the write lock is held, any competing thread will do its work only under the wait
     * list lock. So no need to worry about them once the list is locked.
     */
    lw_assert(old == *lock);
    have_more = find_eligible_waiters(wait_list,
                                      &to_wakeup,
                                      wait_tag.word64,
                                      max_readers,
                                      &next_is_reader);
    lw_assert(lw_dl_get_count(&to_wakeup) > 0 && lw_dl_get_count(&to_wakeup) <= max_readers);
    first = LW_FIELD_2_OBJ(to_wakeup.head, *first, event.iface.link);
    wait_tag.word64 = first->event.tag;
    if (wait_tag.fields.rdlock) {
        new = reader_inc * lw_dl_get_count(&to_wakeup);
    } else {
        new = writer_mask;
    }
    if (have_more) {
        if (wait_tag.fields.rdlock == 0) {
            /* First waiter is a writer with more waiters after that. set reader bit. */
            lw_assert(new == writer_mask && (new & reader_mask) == 0);
            new += reader_inc;
            lw_assert(new == (writer_mask | reader_inc));
        } else if (new == reader_mask) {
            if (next_is_reader) {
                /* Too many readers. Need to wake up those still on the wait list and see if
                 * there is a writer past them.
                 */
                lw_bool_t have_writer;
                have_writer = wake_eligible_readers(wait_list, wait_tag.word64, lock, FALSE);
                if (have_writer) {
                    /* Have a writer waiting after all the readers. */
                    new |= writer_mask;
                }
            } else {
                new |= writer_mask;
            }
        } else {
            new |= writer_mask;
        }
    }
    old = *lock & (reader_mask | writer_mask);
    if (old != new) {
        lw_bool_t swapped;
        swapped = lw_uint32_swap_with_mask(lock, ~(reader_mask | writer_mask), &old, new);
        lw_verify(swapped);
    }
    lw_dl_unlock_writer(wait_list);
    next_is_reader = wait_tag.fields.rdlock;
    while (first != NULL) {
        lw_waiter_t *next = LW_FIELD_2_OBJ_NULL_SAFE(lw_dl_next(&to_wakeup, &first->event.iface.link),
                                                     *next, event.iface.link);
        wait_tag.word64 = first->event.tag;
        wait_tag.fields.in_list = 0;
        wait_tag.fields.have_lock = 1;
        lw_assert(wait_tag.fields.rdlock == next_is_reader);
        first->event.tag = wait_tag.word64;
        lw_dl_remove(&to_wakeup, &first->event.iface.link);
        lw_waiter_wakeup(first, lock);
        first = next;
    }
    lw_dl_destroy(&to_wakeup);
}
