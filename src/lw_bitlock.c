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
lw_bitlock_init(lw_uint32_t num_wait_lists, void *wait_list_memory)
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
lw_bitlock_deinit(void)
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
    do {
        if (old & lock_mask) {
            /* Lock already set. */
            if (!set_wait_bit) {
                return FALSE;
            }
            new = lock_mask | wait_mask;
        } else {
            new = lock_mask;
            lw_assert(!(old & wait_mask));
        }
    } while (!lw_uint32_swap_with_mask(lock, payload_mask, &old, new));

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
    do {
        /* Nothing more. */
        lw_assert(old == (lock_mask | wait_mask));
    } while (!lw_uint32_swap_with_mask(lock, payload_mask, &old, new));
}

/**
 * Acquire a bitlock.
 *
 * @param lock (i/o) the 32-bit word that holds the bits that form the lock.
 * @param lock_mask (i) the bit that represents lock being held.
 * @param wait_mask (i) the bit that is set when waiting.
 */
void
lw_bitlock32_lock(lw_uint32_t *lock, lw_uint32_t lock_mask, lw_uint32_t wait_mask)
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
        return;
    }
    wait_list_idx = LW_BITLOCK_PTR_HASH32(lock);
    wait_list_idx = wait_list_idx % wait_lists_count;
    wait_list = &wait_lists[wait_list_idx];
    waiter = lw_waiter_get();
    lw_dl_lock_writer(wait_list);
    lw_dl_append_at_end(wait_list, &waiter->event.iface.link);
    got_lock = lw_bitlock32_set_lock_bit(lock, lock_mask, wait_mask, TRUE);
    if (got_lock) {
        lw_dl_unlock_writer(wait_list);
        return;
    }
    lw_assert(waiter->event.wait_src == NULL);
    waiter->event.wait_src = lock;
    waiter->event.tag = (lock_mask | wait_mask);
    lw_dl_unlock_writer(wait_list);
    lw_waiter_wait(waiter);
    lw_assert(*lock & lock_mask);
    waiter->event.wait_src = NULL;
    return;
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
 * Try to acquire a bitlock and set the payload to the provided value if the lock is acquired.
 * If the curr_payload pointer is !NULL, the current value is returned regardless of whether
 * the lock is acquired or not.
 *
 * @param lock (i/o) the 32-bit word that holds the bits that form the lock.
 * @param lock_mask (i) the bit that represents lock being held.
 * @param wait_mask (i) the bit that is set when waiting.
 * @param new_payload (i) the value to set payload to.
 * @param curr_payload (o) if not NULL, the current value is returned here.
 * @results 0 if lock is acquired, EBUSY if it cannot due to contention.
 */
lw_int32_t
lw_bitlock32_trylock_set_payload(lw_uint32_t *lock,
                                 LW_IN lw_uint32_t lock_mask,
                                 LW_IN lw_uint32_t wait_mask,
                                 LW_IN lw_uint32_t new_payload,
                                 LW_OUT lw_uint32_t *curr_payload)
{
    lw_uint32_t old, new;
    lw_bool_t swapped = FALSE;

    lw_assert(lock_mask != wait_mask);
    lw_assert(lock_mask != 0 && wait_mask != 0);
    lw_assert(LW_IS_POW2(lock_mask) && LW_IS_POW2(wait_mask));
    lw_assert((lock_mask & new_payload) == 0);
    lw_assert((wait_mask & new_payload) == 0);

    new = new_payload | lock_mask;
    old = *lock;
    if ((old & lock_mask) == 0) {
        lw_assert((old & wait_mask) == 0);
        swapped = lw_uint32_swap(lock, &old, new);
    }
    if (curr_payload != NULL) {
        *curr_payload = old;
    }
    return swapped ? 0 : EBUSY;
}

/**
 * Try to acquire a bitlock only if the payload matches the current value.
 * If the curr_payload pointer is !NULL, the current value is returned regardless of whether
 * the lock is acquired or not.
 *
 * @param lock (i/o) the 32-bit word that holds the bits that form the lock.
 * @param lock_mask (i) the bit that represents lock being held.
 * @param wait_mask (i) the bit that is set when waiting.
 * @param payload (i) the value to set payload to.
 * @param curr_payload (o) if not NULL, the current value is returned here.
 * @results 0 if lock is acquired, EBUSY if it cannot due to contention.
 */
lw_int32_t
lw_bitlock32_trylock_if_payload(lw_uint32_t *lock,
                                LW_IN lw_uint32_t lock_mask,
                                LW_IN lw_uint32_t wait_mask,
                                LW_IN lw_uint32_t payload,
                                LW_OUT lw_uint32_t *curr_payload)
{
    lw_uint32_t new, old;
    lw_bool_t swapped = FALSE;

    lw_assert(lock_mask != wait_mask);
    lw_assert(lock_mask != 0 && wait_mask != 0);
    lw_assert(LW_IS_POW2(lock_mask) && LW_IS_POW2(wait_mask));
    lw_assert((lock_mask & payload) == 0);
    lw_assert((wait_mask & payload) == 0);

    old = payload;
    new = payload | lock_mask;
    swapped = lw_uint32_swap(lock, &old, new);
    if (curr_payload != NULL) {
        *curr_payload = old;
    }
    return swapped ? 0 : EBUSY;
}

/**
 * Release a bitlock. If there are waiters, the 1st one is woken up and the lock handed over
 * to it.
 *
 * @param lock (i/o) the 32-bit word that holds the bits that form the lock.
 * @param lock_mask (i) the bit that represents lock being held.
 * @param wait_mask (i) the bit that is set when waiting.
 */
void
lw_bitlock32_unlock(lw_uint32_t *lock, lw_uint32_t lock_mask, lw_uint32_t wait_mask)
{
    lw_uint32_t wait_list_idx;
    lw_dlist_t *wait_list;
    lw_waiter_t *to_wake_up = NULL;
    lw_delem_t *elem;
    lw_uint32_t mask = lock_mask | wait_mask;
    lw_bool_t multiple_waiters = FALSE;

    lw_assert(lock_mask != wait_mask);
    lw_assert(lock_mask != 0 && wait_mask != 0);
    lw_assert(LW_IS_POW2(lock_mask) && LW_IS_POW2(wait_mask));

    if (lw_bitlock32_drop_lock_if_no_waiters(lock, lock_mask, wait_mask)) {
        /* All done. */
        return;
    }

    wait_list_idx = LW_BITLOCK_PTR_HASH32(lock);
    wait_list_idx = wait_list_idx % wait_lists_count;
    wait_list = &wait_lists[wait_list_idx];

    lw_dl_lock_writer(wait_list);
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
    lw_dl_unlock_writer(wait_list);
    lw_waiter_wakeup(to_wake_up, lock);

    return;
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
    do {
        if (old & lock_mask) {
            /* Lock already set. */
            if (!set_wait_bit) {
                return FALSE;
            }
            new = lock_mask | wait_mask;
        } else {
            new = lock_mask;
            lw_assert(!(old & wait_mask));
        }
    } while (!lw_uint64_swap_with_mask(lock, payload_mask, &old, new));

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
    do {
        /* Nothing more. */
        lw_assert(old == (lock_mask | wait_mask));
    } while (!lw_uint64_swap_with_mask(lock, payload_mask, &old, new));
}

/**
 * Acquire a bitlock.
 *
 * @param lock (i/o) the 64-bit word that holds the bits that form the lock.
 * @param lock_mask (i) the bit that represents lock being held.
 * @param wait_mask (i) the bit that is set when waiting.
 */
void
lw_bitlock64_lock(lw_uint64_t *lock, lw_uint64_t lock_mask, lw_uint64_t wait_mask)
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
        return;
    }
    wait_list_idx = LW_BITLOCK_PTR_HASH32(lock);
    wait_list_idx = wait_list_idx % wait_lists_count;
    wait_list = &wait_lists[wait_list_idx];
    waiter = lw_waiter_get();
    lw_dl_lock_writer(wait_list);
    lw_dl_append_at_end(wait_list, &waiter->event.iface.link);
    got_lock = lw_bitlock64_set_lock_bit(lock, lock_mask, wait_mask, TRUE);
    if (got_lock) {
        lw_dl_unlock_writer(wait_list);
        return;
    }
    lw_assert(waiter->event.wait_src == NULL);
    waiter->event.wait_src = lock;
    waiter->event.tag = (lock_mask | wait_mask);
    lw_dl_unlock_writer(wait_list);
    lw_waiter_wait(waiter);
    lw_assert(*lock & lock_mask);
    waiter->event.wait_src = NULL;
    return;
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
 * Try to acquire a bitlock and set the payload to the provided value if the lock is acquired.
 * If the curr_payload pointer is !NULL, the current value is returned regardless of whether
 * the lock is acquired or not.
 *
 * @param lock (i/o) the 64-bit word that holds the bits that form the lock.
 * @param lock_mask (i) the bit that represents lock being held.
 * @param wait_mask (i) the bit that is set when waiting.
 * @param new_payload (i) the value to set payload to.
 * @param curr_payload (o) if not NULL, the current value is returned here.
 * @results 0 if lock is acquired, EBUSY if it cannot due to contention.
 */
lw_int32_t
lw_bitlock64_trylock_set_payload(lw_uint64_t *lock,
                                 LW_IN lw_uint64_t lock_mask,
                                 LW_IN lw_uint64_t wait_mask,
                                 LW_IN lw_uint64_t new_payload,
                                 LW_OUT lw_uint64_t *curr_payload)
{
    lw_uint64_t old, new;
    lw_bool_t swapped = FALSE;

    lw_assert(lock_mask != wait_mask);
    lw_assert(lock_mask != 0 && wait_mask != 0);
    lw_assert(LW_IS_POW2(lock_mask) && LW_IS_POW2(wait_mask));
    lw_assert((lock_mask & new_payload) == 0);
    lw_assert((wait_mask & new_payload) == 0);

    new = new_payload | lock_mask;
    old = *lock;
    if ((old & lock_mask) == 0) {
        lw_assert((old & wait_mask) == 0);
        swapped = lw_uint64_swap(lock, &old, new);
    }
    if (curr_payload != NULL) {
        *curr_payload = old;
    }
    return swapped ? 0 : EBUSY;
}

/**
 * Try to acquire a bitlock only if the payload matches the current value.
 * If the curr_payload pointer is !NULL, the current value is returned regardless of whether
 * the lock is acquired or not.
 *
 * @param lock (i/o) the 64-bit word that holds the bits that form the lock.
 * @param lock_mask (i) the bit that represents lock being held.
 * @param wait_mask (i) the bit that is set when waiting.
 * @param payload (i) the value to set payload to.
 * @param curr_payload (o) if not NULL, the current value is returned here.
 * @results 0 if lock is acquired, EBUSY if it cannot due to contention.
 */
lw_int32_t
lw_bitlock64_trylock_if_payload(lw_uint64_t *lock,
                                LW_IN lw_uint64_t lock_mask,
                                LW_IN lw_uint64_t wait_mask,
                                LW_IN lw_uint64_t payload,
                                LW_OUT lw_uint64_t *curr_payload)
{
    lw_uint64_t new, old;
    lw_bool_t swapped = FALSE;

    lw_assert(lock_mask != wait_mask);
    lw_assert(lock_mask != 0 && wait_mask != 0);
    lw_assert(LW_IS_POW2(lock_mask) && LW_IS_POW2(wait_mask));
    lw_assert((lock_mask & payload) == 0);
    lw_assert((wait_mask & payload) == 0);

    old = payload;
    new = payload | lock_mask;
    swapped = lw_uint64_swap(lock, &old, new);
    if (curr_payload != NULL) {
        *curr_payload = old;
    }
    return swapped ? 0 : EBUSY;
}

/**
 * Release a bitlock. If there are waiters, the 1st one is woken up and the lock handed over
 * to it.
 *
 * @param lock (i/o) the 64-bit word that holds the bits that form the lock.
 * @param lock_mask (i) the bit that represents lock being held.
 * @param wait_mask (i) the bit that is set when waiting.
 */
void
lw_bitlock64_unlock(lw_uint64_t *lock, lw_uint64_t lock_mask, lw_uint64_t wait_mask)
{
    lw_uint64_t wait_list_idx;
    lw_dlist_t *wait_list;
    lw_waiter_t *to_wake_up = NULL;
    lw_delem_t *elem;
    lw_uint64_t mask = lock_mask | wait_mask;
    lw_bool_t multiple_waiters = FALSE;

    lw_assert(lock_mask != wait_mask);
    lw_assert(lock_mask != 0 && wait_mask != 0);
    lw_assert(LW_IS_POW2(lock_mask) && LW_IS_POW2(wait_mask));

    if (lw_bitlock64_drop_lock_if_no_waiters(lock, lock_mask, wait_mask)) {
        /* All done. */
        return;
    }

    wait_list_idx = LW_BITLOCK_PTR_HASH32(lock);
    wait_list_idx = wait_list_idx % wait_lists_count;
    wait_list = &wait_lists[wait_list_idx];

    lw_dl_lock_writer(wait_list);
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
    lw_dl_unlock_writer(wait_list);
    lw_waiter_wakeup(to_wake_up, lock);

    return;
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

