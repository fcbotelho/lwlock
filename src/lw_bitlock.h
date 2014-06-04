/***
 *
 * Independently contributed to the lwlock library. This file is also released under the
 * terms of MPL 2.0 license but you do not need to sign the committer agreement for
 * this file.
 */

#ifndef __LW_BITLOCK_H__
#define __LW_BITLOCK_H__

#include "lw_types.h"

/*
 * Bitlocks: locks made up of just 2 bits from a 32-bit or 64-bit value. The rest
 * of the bits can be used to store other values. Pointers, indices, hash buckets
 * often have a few unused bits at the top and this can be used to embed locks in
 * them. The caller has to be careful to always only use the APIs provided here to
 * update the bits that are not part of the lock. They are still subject to atomicity
 * requirements and manipulating them directly could result in loss of lock state and
 * that would be very bad.
 *
 * This is a fairly low level library and really meant to be used with other data structures
 * within the library. Use it directly very careful and at your own peril.
 */

void lw_bitlock_init(lw_uint32_t num_wait_lists, void *wait_list_memory);

void lw_bitlock_deinit(void);

typedef struct {
    lw_uint32_t *lock;
    lw_uint32_t lock_mask;
    lw_uint32_t wait_mask;
} lw_bitlock32_spec_t;

void
lw_bitlock32_lock(lw_uint32_t *lock,
                  LW_IN lw_uint32_t lock_mask,
                  LW_IN lw_uint32_t wait_mask,
                  LW_IN lw_bool_t sync);

void lw_bitlock32_lock_complete_wait(lw_uint32_t *lock);

lw_int32_t
lw_bitlock32_trylock(lw_uint32_t *lock, LW_IN lw_uint32_t lock_mask, LW_IN lw_uint32_t wait_mask);

lw_int32_t
lw_bitlock32_trylock_set_payload(lw_uint32_t *lock,
                                 LW_IN lw_uint32_t lock_mask,
                                 LW_IN lw_uint32_t wait_mask,
                                 LW_IN lw_uint32_t new_payload,
                                 LW_OUT lw_uint32_t *curr_payload);

lw_int32_t
lw_bitlock32_trylock_if_payload(lw_uint32_t *lock,
                                LW_IN lw_uint32_t lock_mask,
                                LW_IN lw_uint32_t wait_mask,
                                LW_IN lw_uint32_t payload,
                                LW_OUT lw_uint32_t *curr_payload);

void
lw_bitlock32_unlock(lw_uint32_t *lock, LW_IN lw_uint32_t lock_mask, LW_IN lw_uint32_t wait_mask);

lw_bool_t
lw_bitlock32_swap_payload(lw_uint32_t *lock,
                          lw_uint32_t lock_mask,
                          lw_uint32_t wait_mask,
                          lw_uint32_t *current_payload,
                          lw_uint32_t new_payload);

typedef struct {
    lw_uint64_t *lock;
    lw_uint64_t lock_mask;
    lw_uint64_t wait_mask;
} lw_bitlock64_spec_t;

void
lw_bitlock64_lock(lw_uint64_t *lock,
                  LW_IN lw_uint64_t lock_mask,
                  LW_IN lw_uint64_t wait_mask,
                  LW_IN lw_bool_t sync);

void lw_bitlock64_lock_complete_wait(lw_uint64_t *lock);

lw_int32_t
lw_bitlock64_trylock(lw_uint64_t *lock, LW_IN lw_uint64_t lock_mask, LW_IN lw_uint64_t wait_mask);

lw_int32_t
lw_bitlock64_trylock_set_payload(lw_uint64_t *lock,
                                 LW_IN lw_uint64_t lock_mask,
                                 LW_IN lw_uint64_t wait_mask,
                                 LW_IN lw_uint64_t new_payload,
                                 LW_OUT lw_uint64_t *curr_payload);


void
lw_bitlock64_unlock(lw_uint64_t *lock, LW_IN lw_uint64_t lock_mask, LW_IN lw_uint64_t wait_mask);

lw_bool_t
lw_bitlock64_swap_payload(lw_uint64_t *lock,
                          lw_uint64_t lock_mask,
                          lw_uint64_t wait_mask,
                          lw_uint64_t *current_payload,
                          lw_uint64_t new_payload);

lw_int32_t
lw_bitlock64_trylock_if_payload(lw_uint64_t *lock,
                                LW_IN lw_uint64_t lock_mask,
                                LW_IN lw_uint64_t wait_mask,
                                LW_IN lw_uint64_t payload,
                                LW_OUT lw_uint64_t *curr_payload);

#endif /* __LW_BITLOCK_H__ */
