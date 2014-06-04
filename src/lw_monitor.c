/***
 *
 * Independently contributed to the lwlock library. This file is also released under the
 * terms of MPL 2.0 license but you do not need to sign the committer agreement for
 * this file.
 */

#include "lw_debug.h"
#include "lw_dlist.h"
#include "murmur.h"
#include "lf_stack.h"
#include "lw_bitlock.h"
#include "lw_monitor.h"
#include "lw_atomic.h"

#define MURMUR_SEED                     (0xe35b3018)  /* Something random. */
#define LW_MONITOR_PTR_HASH32(_ptr)     Murmur3Ptr(_ptr, MURMUR_SEED)

/*
 * Monitor locks are assigned from a pool of preallocated lock + condvar structures. The pointer is
 * hashed to a list of locks on which its lock is added if no existing entry for the pointer is
 * found.
 *
 * At a high conceptual level, that is all there is to the monitor abstraction. However, since low
 * contention is expected to be the norm for majority of locks, the overhead to allocate a monitor,
 * init it, add to list, use it, and on unlock with no waiters, return it back to free list sounds
 * unncessarily expensive. In the ideal (and hopefully largely common case), there are a few
 * monitors that will get used often and should hopefully simply retain their mapping. Leaving
 * unused entries associated with their initial pointers, on the other hand, risks running out of
 * monitors and having to do expensive work to track and reclaim unused ones.
 *
 * Here is an intermedaite solution: Using a sufficiently large number of monitors, the pool of
 * available ones can be divided into 2 parts. One is the float and the other is treated like a
 * fixed array: a pointer is hased to a location in the array and --
 *  - If the slot is used by the same pointer already, we have found the lock and use it. This is
 *  hopefully the most common case.
 *  - If the slot is used by some other ptr and is in fact concurrently locked, an entry from the
 * float is taken, associated with the ptr and inserted into the list starting at that fixed
 * location. We now have a lock associated with the ptr.
 * - If the slot is tied to another ptr but is in fact unused, it is taken over and associated with
 *   ptr. This last part also poses challenges with 2 threads try to add a lock for the same ptr and
 *   one does the takeover and the other inserts a new entry. However, this is essential to avoid
 *   having the default fixed slot get lost of one-off monitors over time.
 *
 * The above scheme is basically a hash-table with each bucket head also embedding 1 slot for a
 * value and other slots getting chained from the bucket head.  After the last unlock, if the
 * monitor came from the float, it is taken off the bucket. If it is the fixed monitor, it is left
 * as is and still associated with the ptr under the expectation of repeated use.
 */

typedef struct lw_monitor_s lw_monitor_t;

/*
 * A pointer lock type that assumes a 64-bit address actually fits within 61 bits. True for most
 * (all?) systems really.
 */
typedef union {
    lw_uint64_t atomic64;
    lw_monitor_t *monitor_ptr;
    struct {
        lw_uint64_t lock:1;
        lw_uint64_t wait:1;
        lw_uint64_t cv:1;
        lw_uint64_t ptr61:61;
    } fields;
} lw_monitor_ptrlock_t;

#define LW_MONITOR_PTR_MAX                      ((1ULL << 62) - 1)
#define LW_MONITOR_PTR_IS_ACCEPTABLE(_ptr)      (LW_PTR_2_NUM(_ptr, lw_uint64_t) <= LW_MONITOR_PTR_MAX)
#define LW_MONITOR_PTRLOCK_INITIALIZER          { .atomic64 = 0 }
#define LW_MONITOR_PTRLOCK_IS_LOCKED(_ptr)      ((_ptr)->fields.lock == 1)
#define LW_MONITOR_PTRLOCK_INIT_LOCKED_WITH_PTR(_ptr) \
    { .fields = { .lock = 1, .wait = 0, .cv = 0, .ptr61 = LW_PTR_2_NUM(_ptr, lw_uint64_t) } }

static const lw_monitor_ptrlock_t monitor_lock_bit = { .fields = { .lock = 1, .wait = 0, .cv = 0, .ptr61 = 0 } };
static const lw_monitor_ptrlock_t monitor_wait_bit = { .fields = { .lock = 0, .wait = 1, .cv = 0, .ptr61 = 0 } };
static const lw_monitor_ptrlock_t monitor_cv_bit = { .fields = { .lock = 0, .wait = 0, .cv = 1, .ptr61 = 0 } };

struct lw_monitor_s {
    lw_monitor_ptrlock_t    next;
    lw_monitor_ptrlock_t    ptrlock;
};

static lw_uint32_t monitors_count = 32 * 1024;
static lw_uint32_t fixed_count = 16 * 1024; // 1/2 of monitors_count.
static lw_monitor_t *monitors = NULL;
static lf_stack_t free_monitors;

static void
lw_monitor_init(lw_monitor_t *monitor, void *ptr)
{
    monitor->next.atomic64 = 0;
    if (ptr != NULL) {
        monitor->ptrlock.atomic64 = 0;
        monitor->ptrlock.fields.lock = 1;
        monitor->ptrlock.fields.ptr61 = LW_PTR_2_NUM(ptr, lw_uint64_t);
    } else {
        monitor->ptrlock.atomic64 = 0;
    }
}

static void
lw_monitor_destroy(lw_monitor_t *monitor)
{
    lw_verify(monitor->ptrlock.fields.cv == 0);
    lw_bitlock64_destroy(&monitor->next.atomic64,
                         monitor_lock_bit.atomic64,
                         monitor_wait_bit.atomic64);
    lw_verify(monitor->next.fields.ptr61 == 0);
    lw_bitlock64_destroy(&monitor->ptrlock.atomic64,
                         monitor_lock_bit.atomic64,
                         monitor_wait_bit.atomic64);
}

/**
 * Init monitor module.
 *
 * This function has to be called before any monitors can be operated on. It initializes
 * the lists where threads wait in contention case.
 *
 * @param num_monitor_lists (i) the number of wait lists to use.
 * @param wait_list_memory (i) if !NULL, this is used for the lists. The caller has to
 * ensure the region is large enough for the number of lists desired.
 */
void
lw_monitor_module_init(lw_uint32_t num_monitors)
{
    lw_uint32_t i;
    if (num_monitors != 0) {
        monitors_count = num_monitors;
    }
    monitors = malloc(monitors_count * sizeof(lw_monitor_t));
    fixed_count = monitors_count / 2;
    lw_verify(LW_MONITOR_PTR_IS_ACCEPTABLE(&monitors[monitors_count - 1]));
    for (i = 0; i < monitors_count; i++) {
        lw_monitor_t *monitor = monitors + i;
        lw_monitor_init(monitor, NULL);
    }
    lf_stack_init(&free_monitors,
                  (lw_uint8_t *)&monitors[fixed_count],
                  monitors_count - fixed_count,
                  sizeof(lw_monitor_t),
                  TRUE);
}

void
lw_monitor_module_deinit(void)
{
    /*
     * Just check the fixed elements. No current locks held and all
     * next pointers being NULL imply the rest are also unused.
     */
    lw_uint32_t i;
    for (i = 0; i < fixed_count; i++) {
        lw_monitor_t *monitor = monitors + i;
        lw_monitor_destroy(monitor);
    }
    free(monitors);
}

static lw_monitor_id_t
monitor_get_id(lw_monitor_t *monitor)
{
    lw_uint64_t idx = monitor - monitors;
    return idx;
}

static lw_monitor_t *
monitor_from_id(lw_monitor_id_t id)
{
    lw_uint32_t idx = id;
    lw_monitor_t *monitor = &monitors[idx];
    return monitor;
}

static lw_monitor_t *
lw_monitor_alloc(void *ptr)
{
    lw_monitor_t *monitor = lf_stack_pop(&free_monitors);
    lw_assert(monitor != NULL);
    lw_monitor_init(monitor, ptr);
    return monitor;
}

static void
lw_monitor_release(lw_monitor_t *monitor)
{
    monitor->ptrlock.fields.ptr61 = 0;
    lw_bitlock64_unlock(&monitor->ptrlock.atomic64,
                        monitor_lock_bit.atomic64,
                        monitor_wait_bit.atomic64);
    lw_monitor_destroy(monitor);
    lf_stack_push(&free_monitors, monitor);
}

static lw_bool_t
monitor_ptr_lock_async(lw_monitor_ptrlock_t *ptrlock)
{
    return lw_bitlock64_lock_async(&ptrlock->atomic64,
                                   monitor_lock_bit.atomic64,
                                   monitor_wait_bit.atomic64);
}

static void
monitor_ptr_lock_complete_wait(lw_monitor_ptrlock_t *ptrlock)
{
    lw_bitlock_complete_wait(&ptrlock->atomic64);
}

static void
monitor_ptr_lock(lw_monitor_ptrlock_t *ptrlock)
{
    lw_bitlock64_lock(&ptrlock->atomic64, monitor_lock_bit.atomic64, monitor_wait_bit.atomic64);
}

static void
monitor_ptr_unlock(lw_monitor_ptrlock_t *ptrlock)
{
    lw_bitlock64_unlock(&ptrlock->atomic64, monitor_lock_bit.atomic64, monitor_wait_bit.atomic64);
}

static lw_waiter_t *
monitor_ptr_unlock_return_waiter(lw_monitor_ptrlock_t *ptrlock)
{
    return lw_bitlock64_unlock_return_waiter(&ptrlock->atomic64,
                                             monitor_lock_bit.atomic64,
                                             monitor_wait_bit.atomic64);
}

static void
monitor_move_users(lw_monitor_t *curr, lw_monitor_t *new)
{
    lw_bitlock64_rekey(&curr->ptrlock.atomic64, &new->ptrlock.atomic64,
                       monitor_lock_bit.atomic64, monitor_wait_bit.atomic64,
                       monitor_cv_bit.atomic64);
}

/**
 * Currently only called with bitlock on chain head held.
 * Since the head of the chain can still change its wait bits, the function still uses
 * atomics to manipulate values. We can optimize it a little when operating on non-head
 * elements.
 */
static lw_monitor_t *
find_monitor_for_ptr(void *ptr,
                     lw_monitor_t *monitor,
                     lw_bool_t skip_head_from_search,
                     lw_monitor_t *to_add_if_not_found,
                     lw_monitor_t **prev)
{
    lw_uint64_t val_to_find = LW_PTR_2_NUM(ptr, lw_uint64_t);
    lw_monitor_ptrlock_t to_insert;
    *prev = monitor;
    to_insert.fields.ptr61 = LW_PTR_2_NUM(to_add_if_not_found, lw_uint64_t);

    lw_assert(LW_MONITOR_PTRLOCK_IS_LOCKED(&monitor->next));

    if (skip_head_from_search) {
        if (monitor->next.fields.ptr61 == 0 && to_add_if_not_found != NULL) {
            /* No link in chain after head. Add the new link in. */
            lw_uint64_t mask = monitor_lock_bit.atomic64 | monitor_wait_bit.atomic64;
            lw_uint64_t old = 0;
            lw_bool_t inserted;
            inserted = lw_uint64_swap_with_mask(&monitor->next.atomic64, mask, &old,
                                                to_insert.atomic64);
            lw_assert(inserted);
            LW_UNUSED_PARAMETER(inserted);
            return to_add_if_not_found;
        }
        *prev = monitor; // Redundant due to assignment at top of function but keeps clean.
        monitor = LW_NUM_2_PTR(monitor->next.fields.ptr61, *monitor);
    }

    while (monitor != NULL) {

        if (monitor->ptrlock.fields.ptr61 == val_to_find) {
            return monitor;
        }

        if (monitor->next.fields.ptr61 == 0 && to_add_if_not_found != NULL) {
            lw_uint64_t mask = monitor_lock_bit.atomic64 | monitor_wait_bit.atomic64;
            lw_uint64_t old = 0;
            lw_bool_t inserted;
            inserted = lw_uint64_swap_with_mask(&monitor->next.atomic64, mask, &old,
                                                to_insert.atomic64);
            lw_assert(inserted);
            LW_UNUSED_PARAMETER(inserted);
        }
        *prev = monitor;
        monitor = LW_NUM_2_PTR(monitor->next.fields.ptr61, *monitor);
    }

    return NULL;
}

static inline lw_bool_t
head_is_only_monitor(void *ptr, lw_monitor_t *monitor)
{
    lw_monitor_t *prev;
    lw_monitor_t *other;
    monitor_ptr_lock(&monitor->next);
    other = find_monitor_for_ptr(ptr, monitor, TRUE, NULL, &prev);
    lw_assert(other == NULL);
    monitor_ptr_unlock(&monitor->next);
    return other == NULL;
}

/*
 * Check if the given monitor is free to be reused or is already mapped to the given pointer
 * and unlocked. If so, lock it and return true.
 *
 * If the monitor is already locked or there is a waiter on it for some other pointer, then it is
 * in use and the function will return false.
 */
static lw_bool_t
lw_monitor_grab_if_unused(lw_monitor_t *monitor, void *ptr, lw_uint64_t *previous_owner)
{
    lw_monitor_ptrlock_t old = LW_MONITOR_PTRLOCK_INITIALIZER;
    lw_monitor_ptrlock_t new = LW_MONITOR_PTRLOCK_INIT_LOCKED_WITH_PTR(ptr);

    old.atomic64 = monitor->ptrlock.atomic64;
    do {
        if (old.fields.cv == 1 && old.fields.ptr61 != new.fields.ptr61) {
            *previous_owner = old.fields.ptr61;
            return FALSE; // Monitor in use by some other thread.
        }
        if (old.fields.lock == 1) {
            *previous_owner = old.fields.ptr61;
            return FALSE; // Locked.
        }
        lw_assert(old.fields.wait == 0);
        new.fields.cv = old.fields.cv;
    } while (!lw_uint64_swap(&monitor->ptrlock.atomic64, &old.atomic64, new.atomic64));

    *previous_owner = old.fields.ptr61;
    return TRUE;
}

static lw_bool_t
lw_monitor_lock_if_ptr_matches(lw_monitor_t *monitor, void *ptr, lw_bool_t sync)
{
    lw_monitor_ptrlock_t old = LW_MONITOR_PTRLOCK_INITIALIZER;
    lw_uint64_t ptr61 = LW_PTR_2_NUM(ptr, lw_uint64_t);

    old.fields.ptr61 = ptr61;
    do {
        if (old.fields.ptr61 != ptr61) {
            return FALSE;
        }
    } while (!lw_bitlock64_lock_if_payload(&monitor->ptrlock.atomic64,
                                           monitor_lock_bit.atomic64,
                                           monitor_wait_bit.atomic64,
                                           &old.atomic64, sync));
    return TRUE;
}

lw_monitor_id_t
lw_monitor_lock(void *ptr)
{
    lw_uint32_t hash = LW_MONITOR_PTR_HASH32(ptr);
    lw_uint32_t slot = hash % fixed_count;
    lw_monitor_t * const monitor = &monitors[slot];
    lw_bool_t got_monitor;
    lw_uint64_t previous_owner; // For debugging.
    lw_uint32_t loop = 0;


    /*
     * Disallow NULL pointers. Using it is a lazy form of a global lock and we can't tell whether
     * ptr61 holds legit values or not.
     */
    lw_assert(ptr != NULL);
    lw_verify(LW_MONITOR_PTR_IS_ACCEPTABLE(ptr));

top:
    got_monitor = lw_monitor_grab_if_unused(monitor, ptr, &previous_owner);
    if (got_monitor && monitor->next.monitor_ptr == NULL) {
        /*
         * Common case: Got lock from the chain head and there is no other link or
         * on-going activity related to the chain links.
         */
        lw_assert(head_is_only_monitor(ptr, monitor));
        return monitor_get_id(monitor);
    }

    monitor_ptr_lock(&monitor->next);

    if (got_monitor) {
        /* Did get lock. This is checking to see if another thread had inserted a copy in the chain. */
        lw_monitor_t *prev;
        lw_monitor_t *another_copy = find_monitor_for_ptr(ptr, monitor, TRUE, NULL, &prev);
        if (another_copy) {
            /*
             * Need to wait for this 2nd link to go away. Any new callers are going to block on the
             * head of the chain which this thread already owns. So all we need is to take this 2nd
             * lock and dispose of it.
             */
            lw_bool_t popped;
            lw_monitor_ptrlock_t new_after_swap, old;
            lw_uint64_t mask = monitor_lock_bit.atomic64 | monitor_wait_bit.atomic64 |
                               monitor_cv_bit.atomic64;
            new_after_swap.fields.ptr61 = another_copy->next.fields.ptr61;
            old.fields.ptr61 = LW_PTR_2_NUM(another_copy, lw_uint64_t);
            popped = lw_uint64_swap_with_mask(&prev->next.atomic64, mask, &old.atomic64,
                                              new_after_swap.atomic64);
            lw_verify(popped);

            another_copy->next.fields.ptr61 = 0;
            const lw_bool_t got_lock = monitor_ptr_lock_async(&another_copy->ptrlock);
            monitor_ptr_unlock(&monitor->next);
            if (!got_lock) {
                monitor_ptr_lock_complete_wait(&another_copy->ptrlock);
            }
            monitor_move_users(another_copy, monitor);
            lw_monitor_release(another_copy);
        } else {
            /* Other monitors on chain refer to other pointers. */
            monitor_ptr_unlock(&monitor->next);
        }
        return monitor_get_id(monitor);
    } else if (lw_monitor_lock_if_ptr_matches(monitor, ptr, FALSE)) {
        /*
         * Did not get lock right away but that was because the monitor was
         * locked.
         */
        monitor_ptr_unlock(&monitor->next);
        monitor_ptr_lock_complete_wait(&monitor->ptrlock);
        return monitor_get_id(monitor);
    } else {
        lw_monitor_t *monitor_to_add;
        lw_monitor_t *prev;
        lw_monitor_t *existing_monitor;

        monitor_to_add = lw_monitor_alloc(ptr);
        existing_monitor = find_monitor_for_ptr(ptr, monitor, FALSE, monitor_to_add, &prev);
        lw_verify(existing_monitor != NULL);
        if (existing_monitor == monitor) {
            /* The head of chain has become the one after all. */
            monitor_ptr_unlock(&monitor->next);
            lw_monitor_release(monitor_to_add);
            loop++;
            lw_assert(loop < 128); // Just a hueristic guess.
            goto top;
        } else if (existing_monitor == monitor_to_add) {
            /* New monitor added. Nothing more to do. */
            /* XXX: not quite... need to recheck monitor here i think. */
            monitor_ptr_unlock(&monitor->next);
            lw_assert(existing_monitor->ptrlock.fields.lock == 1);
            return monitor_get_id(monitor_to_add);
        } else {
            /* Found an existing entry. */
            lw_monitor_release(monitor_to_add);
            const lw_bool_t got_lock = monitor_ptr_lock_async(&existing_monitor->ptrlock);
            monitor_ptr_unlock(&monitor->next);
            if (!got_lock) {
                monitor_ptr_lock_complete_wait(&existing_monitor->ptrlock);
            }
            return monitor_get_id(existing_monitor);
        }
    }
    lw_verify(FALSE);
    return 0;
}

void
lw_monitor_unlock(void *ptr, lw_monitor_id_t id)
{
    lw_monitor_t *monitor = monitor_from_id(id);
    lw_uint64_t ptr64 = LW_PTR_2_NUM(ptr, lw_uint64_t);
    lw_monitor_ptrlock_t current;
    lw_waiter_t *new_owner;
    lw_bool_t popped;
    lw_monitor_t *prev;
    lw_uint32_t hash;
    lw_uint32_t slot;
    lw_monitor_t *chain_head;
    lw_monitor_t *in_chain;

    current.atomic64 = monitor->ptrlock.atomic64;
    lw_assert(current.fields.ptr61 == ptr64);
    new_owner = monitor_ptr_unlock_return_waiter(&monitor->ptrlock);
    if (new_owner != NULL) {
        lw_waiter_wakeup(new_owner, &monitor->ptrlock.atomic64);
        /* Nothing more to do. */
        return;
    }
    if (current.fields.cv == 1) {
        /* Still in use. */
        return;
    }
    if (id < fixed_count) {
        /* The monitor was part of the fixed set. Again, nothing more to do. */
        return;
    }
    /* Potentially can free the slot. Try it. */
    hash = LW_MONITOR_PTR_HASH32(ptr);
    slot = hash % fixed_count;
    chain_head = &monitors[slot];
    /* XXX/TODO: What if the system is already shut down at this point? */
    monitor_ptr_lock(&chain_head->next);
    in_chain = find_monitor_for_ptr(ptr, chain_head, TRUE, NULL, &prev);
    if (in_chain != monitor) {
        /* Element removed already by some other thread. */
        monitor_ptr_unlock(&chain_head->next);
        return;
    } else {
        popped = FALSE;
        current.atomic64 = in_chain->ptrlock.atomic64;
        current.fields.ptr61 = 0;
        if (current.atomic64 == 0) {
            lw_monitor_ptrlock_t new_after_swap, old;
            lw_uint64_t mask = monitor_lock_bit.atomic64 | monitor_wait_bit.atomic64 |
                               monitor_cv_bit.atomic64;
            old.fields.ptr61 = LW_PTR_2_NUM(in_chain, lw_uint64_t);
            new_after_swap.fields.ptr61 = in_chain->next.fields.ptr61;
            popped = lw_uint64_swap_with_mask(&prev->next.atomic64, mask, &old.atomic64,
                                              new_after_swap.atomic64);
        }
        monitor_ptr_unlock(&chain_head->next);
        if (popped) {
            in_chain->ptrlock.fields.lock = 1; // Set for lw_monitor_release as it expects it.
            lw_monitor_release(in_chain);
        }
        return;
    }
}

void
lw_monitor_wait(void *ptr, lw_monitor_id_t *id)
{
    lw_monitor_t *monitor = monitor_from_id(*id);
    lw_waiter_t *waiter = lw_waiter_get();
    lw_uint64_t *lock_woken_from;

    lw_assert(monitor->ptrlock.fields.ptr61 == LW_PTR_2_NUM(ptr, lw_uint64_t));
    /* XXX: need to know here which lock this woke up with... */
    lw_bitlock64_cv_wait(&monitor->ptrlock.atomic64,
                         monitor_lock_bit.atomic64,
                         monitor_wait_bit.atomic64,
                         monitor_cv_bit.atomic64,
                         FALSE);
    lw_waiter_wait(waiter);
    lock_woken_from = waiter->event.wait_src;
    lw_waiter_clear_src(waiter);
    if (lock_woken_from == &monitor->ptrlock.atomic64) {
        return;
    }
    /* The monitor to use got remapped. */
    monitor = LW_FIELD_2_OBJ(lock_woken_from, *monitor, ptrlock.atomic64);
    *id = monitor_get_id(monitor);
}

void
lw_monitor_signal(void *ptr, lw_monitor_id_t id)
{
    lw_monitor_t *monitor = monitor_from_id(id);

    lw_assert(monitor->ptrlock.fields.ptr61 == LW_PTR_2_NUM(ptr, lw_uint64_t));
    lw_bitlock64_cv_signal(&monitor->ptrlock.atomic64,
                           monitor_lock_bit.atomic64,
                           monitor_wait_bit.atomic64,
                           monitor_cv_bit.atomic64);

}

void
lw_monitor_broadcast(void *ptr, lw_monitor_id_t id)
{
    lw_monitor_t *monitor = monitor_from_id(id);

    lw_assert(monitor->ptrlock.fields.ptr61 == LW_PTR_2_NUM(ptr, lw_uint64_t));
    lw_bitlock64_cv_broadcast(&monitor->ptrlock.atomic64,
                              monitor_lock_bit.atomic64,
                              monitor_wait_bit.atomic64,
                              monitor_cv_bit.atomic64);

}
