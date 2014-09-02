/***
 *
 * Independently contributed to the lwlock library. This file is also released under the
 * terms of MPL 2.0 license but you do not need to sign the committer agreement for
 * this file.
 */

#ifndef __LW_BITLOCK_H__
#define __LW_BITLOCK_H__

#include "lw_types.h"
#include "lw_waiter.h"

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

void lw_bitlock_module_init(lw_uint32_t num_wait_lists, void *wait_list_memory);

void lw_bitlock_module_deinit(void);

typedef enum {
    LW_BITLOCK32_MUTEX,
    LW_BITLOCK32_WRITER,
    LW_BITLOCK32_READER,
    LW_BITLOCK64_MUTEX,
    LW_BITLOCK64_WRITER,
    LW_BITLOCK64_READER,
} lw_bitlock_type_t;

typedef struct {
    lw_uint32_t *lock;
    lw_uint32_t lock_mask;
    lw_uint32_t wait_mask;
    lw_uint32_t num_bits;
    lw_uint32_t start_bit;
    lw_uint8_t  waiter_buf[0];  // For reader lock in reader-writer bitlocks.
} lw_bitlock32_spec_t;

typedef struct {
    lw_uint64_t *lock;
    lw_uint64_t lock_mask;
    lw_uint64_t wait_mask;
    lw_uint32_t num_bits;
    lw_uint32_t start_bit;
    lw_uint8_t  waiter_buf[0];  // For reader lock in reader-writer bitlocks.
} lw_bitlock64_spec_t;

typedef struct {
    lw_bitlock_type_t lock_type;
    union {
        lw_bitlock32_spec_t spec32;
        lw_bitlock64_spec_t spec64;
    } u;
} lw_bitlock_spec_t;

void
lw_bitlock32_init(lw_uint32_t *lock,
                  LW_IN lw_uint32_t lock_mask,
                  LW_IN lw_uint32_t wait_mask);

void
lw_bitlock32_destroy(lw_uint32_t *lock,
                     LW_IN lw_uint32_t lock_mask,
                     LW_IN lw_uint32_t wait_mask);

lw_bool_t
lw_bitlock32_lock_if_payload(lw_uint32_t *lock,
                             LW_IN lw_uint32_t lock_mask,
                             LW_IN lw_uint32_t wait_mask,
                             lw_uint32_t *payload,
                             LW_IN lw_bool_t sync);
lw_bool_t
lw_bitlock32_lock_async(lw_uint32_t *lock,
                        LW_IN lw_uint32_t lock_mask,
                        LW_IN lw_uint32_t wait_mask);

void lw_bitlock_complete_wait(void *lock);

static inline void ALWAYS_INLINE
lw_bitlock32_lock(lw_uint32_t *lock,
                  LW_IN lw_uint32_t lock_mask,
                  LW_IN lw_uint32_t wait_mask)
{
    lw_bool_t got_lock = lw_bitlock32_lock_async(lock, lock_mask, wait_mask);
    if (!got_lock) {
        lw_bitlock_complete_wait(lock);
    }
}

void
lw_bitlock32_cv_wait(lw_uint32_t *lock,
                     LW_IN lw_uint32_t lock_mask,
                     LW_IN lw_uint32_t wait_mask,
                     LW_IN lw_uint32_t cv_mask,
                     LW_IN lw_bool_t sync);

void
lw_bitlock32_cv_signal(lw_uint32_t *lock,
                       LW_IN lw_uint32_t lock_mask,
                       LW_IN lw_uint32_t wait_mask,
                       LW_IN lw_uint32_t cv_mask);

void
lw_bitlock32_cv_broadcast(lw_uint32_t *lock,
                          LW_IN lw_uint32_t lock_mask,
                          LW_IN lw_uint32_t wait_mask,
                          LW_IN lw_uint32_t cv_mask);

lw_int32_t
lw_bitlock32_trylock(lw_uint32_t *lock, LW_IN lw_uint32_t lock_mask, LW_IN lw_uint32_t wait_mask);

lw_int32_t
lw_bitlock32_trylock_cmpxchng_payload(lw_uint32_t *lock,
                                      LW_IN lw_uint32_t lock_mask,
                                      LW_IN lw_uint32_t wait_mask,
                                      LW_INOUT lw_uint32_t *curr_payload,
                                      LW_IN lw_uint32_t new_payload);

lw_waiter_t *
lw_bitlock32_unlock_return_waiter(lw_uint32_t *lock,
                                  LW_IN lw_uint32_t lock_mask,
                                  LW_IN lw_uint32_t wait_mask);

static inline void ALWAYS_INLINE
lw_bitlock32_unlock(lw_uint32_t *lock, LW_IN lw_uint32_t lock_mask, LW_IN lw_uint32_t wait_mask)
{
    lw_waiter_t *waiter = lw_bitlock32_unlock_return_waiter(lock, lock_mask, wait_mask);
    if (waiter != NULL) {
        lw_waiter_wakeup(waiter, lock);
    }
}

lw_bool_t
lw_bitlock32_swap_payload(lw_uint32_t *lock,
                          lw_uint32_t lock_mask,
                          lw_uint32_t wait_mask,
                          lw_uint32_t *current_payload,
                          lw_uint32_t new_payload);

void
lw_bitlock64_init(lw_uint64_t *lock,
                  LW_IN lw_uint64_t lock_mask,
                  LW_IN lw_uint64_t wait_mask);

void
lw_bitlock64_destroy(lw_uint64_t *lock,
                     LW_IN lw_uint64_t lock_mask,
                     LW_IN lw_uint64_t wait_mask);

lw_bool_t
lw_bitlock64_lock_async(lw_uint64_t *lock,
                        LW_IN lw_uint64_t lock_mask,
                        LW_IN lw_uint64_t wait_mask);

static inline void ALWAYS_INLINE
lw_bitlock64_lock(lw_uint64_t *lock,
                  LW_IN lw_uint64_t lock_mask,
                  LW_IN lw_uint64_t wait_mask)
{
    lw_bool_t got_lock = lw_bitlock64_lock_async(lock, lock_mask, wait_mask);
    if (!got_lock) {
        lw_bitlock_complete_wait(lock);
    }
}

lw_bool_t
lw_bitlock64_lock_if_payload(lw_uint64_t *lock,
                             LW_IN lw_uint64_t lock_mask,
                             LW_IN lw_uint64_t wait_mask,
                             lw_uint64_t *payload,
                             LW_IN lw_bool_t sync);

lw_int32_t
lw_bitlock64_trylock(lw_uint64_t *lock, LW_IN lw_uint64_t lock_mask, LW_IN lw_uint64_t wait_mask);

lw_int32_t
lw_bitlock64_trylock_cmpxchng_payload(lw_uint64_t *lock,
                                      LW_IN lw_uint64_t lock_mask,
                                      LW_IN lw_uint64_t wait_mask,
                                      LW_INOUT lw_uint64_t *curr_payload,
                                      LW_IN lw_uint64_t new_payload);

lw_waiter_t *
lw_bitlock64_unlock_return_waiter(lw_uint64_t *lock,
                                  LW_IN lw_uint64_t lock_mask,
                                  LW_IN lw_uint64_t wait_mask);

static inline void ALWAYS_INLINE
lw_bitlock64_unlock(lw_uint64_t *lock, LW_IN lw_uint64_t lock_mask, LW_IN lw_uint64_t wait_mask)
{
    lw_waiter_t *waiter = lw_bitlock64_unlock_return_waiter(lock, lock_mask, wait_mask);
    if (waiter != NULL) {
        lw_waiter_wakeup(waiter, lock);
    }
}

lw_bool_t
lw_bitlock64_swap_payload(lw_uint64_t *lock,
                          lw_uint64_t lock_mask,
                          lw_uint64_t wait_mask,
                          lw_uint64_t *current_payload,
                          lw_uint64_t new_payload);

void
lw_bitlock64_cv_wait(lw_uint64_t *lock,
                     LW_IN lw_uint64_t lock_mask,
                     LW_IN lw_uint64_t wait_mask,
                     LW_IN lw_uint64_t cv_mask,
                     LW_IN lw_bool_t sync);

void
lw_bitlock64_cv_signal(lw_uint64_t *lock,
                       LW_IN lw_uint64_t lock_mask,
                       LW_IN lw_uint64_t wait_mask,
                       LW_IN lw_uint64_t cv_mask);

void
lw_bitlock64_cv_broadcast(lw_uint64_t *lock,
                          LW_IN lw_uint64_t lock_mask,
                          LW_IN lw_uint64_t wait_mask,
                          LW_IN lw_uint64_t cv_mask);

void
lw_bitlock64_rekey(LW_INOUT lw_uint64_t *lock,
                   LW_INOUT lw_uint64_t *newlock,
                   LW_IN lw_uint64_t lock_mask,
                   LW_IN lw_uint64_t wait_mask,
                   LW_IN lw_uint64_t cv_mask);

/*
 * Reader-writer locks made out of a few bits of a word. This is useful for using unused bits of
 * a 32 bit or 64 bit word as a lock.
 *
 * The lock is specified by providing the starting bit and the number of bits to use.  The bits
 * to use as a lock are specified as follows: The LSB of the range given is treated as the write
 * lock bit. The other bits count the number of readers.  When the reader count is saturated,
 * the "overflow" of reader count is tracked separately. This means that the cost of read
 * lock/unlock will increase once the reader bits are saturated. It is up to the user to pick a
 * suitable number of bits to avoid running into the saturated state as much as possible.
 *
 * To make the most efficient use of the bits, a separate wait bit is not kept. The lock is
 * always fair and whether the read or write lock is currently held is inferred from the
 * behavior of the threads. The bit themselves can have both writer and reader bits set. Which
 * lock is really held and who is waiting cannot be determined from the bits themselves but only
 * from looking at the waiter state maintained interally.
 *
 * The following rules are applied:
 *
 * Writer lock is attempted --
 *  - reader count is 0: set writer bit atomically. done.
 *  - reader count is > 0: Set writer bit + add waiter structure to the internally kept list.
 *      There could already be entries from saturated readers in the wait list. Add to the end.
 *  - writer bit is already set: Same as reader > 0. Add to wait list. However, if the reader
 *      count is 0, it is set to 1 so that the unlock path will try to do lock handoff work.
 *
 * Writer unlock --
 *  - reader count is 0: no contention. Writer bit cleared. done.
 *  - reader count is > 0: Contention case. Look at the wait list to see if readers or writers
 *      are waiting.  Based on what is found, the reader and writer bits are set for the next round.
 *
 *
 * Reader lock --
 *  - writer bit set: add to wait list, set reader count to 1 if 0. Else let it be.
 *  - reader count not saturated: increment count, done.
 *  - reader count saturated: add struct to wait list (to account for overflow readers), leave
 *      count as is. Don't actually wait.
 *
 * Reader unlock --
 *  - writer bit not set, reader count not saturated: expected normal case. Decrement count, return.
 *  - writer bit not set, reader count saturated: Check the wait list. First waiter should be a
 *      reader or the list should be empty. If reader found, unlink it and leave count as is.
 *      Otherwise, decrement count.
 *  - writer bit set, reader count not saturated: decrement reader count if > 2. If 1, also
 *      check the wait list. First waiter should be for a write. If there is one more waiter after
 *      that, regardless of type, the reader count is left at 1. Otherwise it is set to 0.
 *  - writer bit set, reader count saturated: Check the wait list. If first waiter is a reader,
 *      proceed as in above case of writer bit not set by unlinking reader and leaving count alone.
 *      If first waiter is a writer, decrement reader count.
 *
 * In the above scenarios, if the number of bits given to the lock is 2, then only 1 bit is
 * available for reader counts. That would make setting reader count to 1 or saturating it to be
 * the same thing. Some of the above checks will require very careful construction. Instead,
 * lets just say that at least 3 bits should be provided and leave the ability to have 2 bits
 * for some other day.
 */
void
lw_bitlock32_rwlock_init(lw_uint32_t *lock,
                         LW_IN lw_uint32_t start_bit,
                         LW_IN lw_uint32_t num_bits);

void
lw_bitlock32_rwlock_destroy(lw_uint32_t *lock,
                            LW_IN lw_uint32_t start_bit,
                            LW_IN lw_uint32_t num_bits);

lw_bool_t
lw_bitlock32_readlock_async(lw_uint32_t *lock,
                            LW_IN lw_uint32_t start_bit,
                            LW_IN lw_uint32_t num_bits,
                            LW_IN lw_bool_t try_lock,
                            lw_uint8_t *waiter_buf);

void
lw_bitlock_rw_complete_wait(lw_uint8_t *waiter_buf);

static inline void ALWAYS_INLINE
lw_bitlock32_readlock(lw_uint32_t *lock,
                      LW_IN lw_uint32_t start_bit,
                      LW_IN lw_uint32_t num_bits,
                      lw_uint8_t *waiter_buf)
{
    lw_bool_t got_lock = lw_bitlock32_readlock_async(lock, start_bit, num_bits, FALSE, waiter_buf);
    if (!got_lock) {
        lw_bitlock_rw_complete_wait(waiter_buf);
    }
}

lw_int32_t
lw_bitlock32_try_readlock(lw_uint32_t *lock,
                          LW_IN lw_uint32_t start_bit,
                          LW_IN lw_uint32_t num_bits,
                          lw_uint8_t *waiter_buf);

void
lw_bitlock32_readunlock(lw_uint32_t *lock,
                        LW_IN lw_uint32_t start_bit,
                        LW_IN lw_uint32_t num_bits,
                        lw_uint8_t *waiter_buf);

void
with_bitlock_reader(lw_uint32_t *lock,
                    LW_IN lw_uint32_t start_bit,
                    LW_IN lw_uint32_t num_bits,
                    callback_func_t callback,
                    void *arg);

lw_bool_t
lw_bitlock32_writelock_async(lw_uint32_t *lock,
                             LW_IN lw_uint32_t start_bit,
                             LW_IN lw_uint32_t num_bits,
                             LW_IN lw_bool_t try_lock);

static inline void ALWAYS_INLINE
lw_bitlock32_writelock(lw_uint32_t *lock,
                       LW_IN lw_uint32_t start_bit,
                       LW_IN lw_uint32_t num_bits)
{
    lw_bool_t got_lock = lw_bitlock32_writelock_async(lock, start_bit, num_bits, FALSE);
    if (!got_lock) {
        lw_bitlock_rw_complete_wait((lw_uint8_t *)lw_waiter_get());
    }
}

lw_int32_t
lw_bitlock32_try_writelock(lw_uint32_t *lock,
                           LW_IN lw_uint32_t start_bit,
                           LW_IN lw_uint32_t num_bits);

void
lw_bitlock32_writeunlock(lw_uint32_t *lock,
                         LW_IN lw_uint32_t start_bit,
                         LW_IN lw_uint32_t num_bits);

static INLINE void
lw_bitlock_lock(lw_bitlock_spec_t *spec)
{
    switch (spec->lock_type) {
        case LW_BITLOCK32_MUTEX:
            lw_bitlock32_lock(spec->u.spec32.lock, spec->u.spec32.lock_mask, spec->u.spec32.wait_mask);
            break;
        case LW_BITLOCK32_WRITER:
            lw_bitlock32_writelock(spec->u.spec32.lock, spec->u.spec32.start_bit,
                                   spec->u.spec32.num_bits);
            break;
        case LW_BITLOCK32_READER:
            lw_bitlock32_readlock(spec->u.spec32.lock, spec->u.spec32.start_bit,
                                  spec->u.spec32.num_bits, spec->u.spec32.waiter_buf);
            break;
        case LW_BITLOCK64_MUTEX:
            lw_bitlock64_lock(spec->u.spec64.lock, spec->u.spec64.lock_mask, spec->u.spec64.wait_mask);
            break;
        default:
            lw_verify(FALSE); // Unknown or unimplemented type.
    }
}


static INLINE void
lw_bitlock_unlock(lw_bitlock_spec_t *spec)
{
    switch (spec->lock_type) {
        case LW_BITLOCK32_MUTEX:
            lw_bitlock32_unlock(spec->u.spec32.lock, spec->u.spec32.lock_mask, spec->u.spec32.wait_mask);
            break;
        case LW_BITLOCK32_WRITER:
            lw_bitlock32_writeunlock(spec->u.spec32.lock, spec->u.spec32.start_bit,
                                     spec->u.spec32.num_bits);
            break;
        case LW_BITLOCK32_READER:
            lw_bitlock32_readunlock(spec->u.spec32.lock, spec->u.spec32.start_bit,
                                    spec->u.spec32.num_bits, spec->u.spec32.waiter_buf);
            break;
        case LW_BITLOCK64_MUTEX:
            lw_bitlock64_unlock(spec->u.spec64.lock, spec->u.spec64.lock_mask, spec->u.spec64.wait_mask);
            break;
        default:
            lw_verify(FALSE); // Unknown or unimplemented type.
    }
}

#endif /* __LW_BITLOCK_H__ */
