#ifndef __LW_MUTEX_H__
#define __LW_MUTEX_H__

#include "lw_types.h"


/*
 * Lightweight mutex.
 *
 * Has the same functionality as a regular mutex. We aren't using the lw_rwlock
 * (which is a read-write lock) for this as that only has a write lock bit but
 * doesn't track the current owner. Since we don't need a reader count, we can
 * track the owner and hence do deadlock detection in the version below. If
 * someday we decide to increase lw_rwlock size to be 8 bytes and track the
 * writer thread waiter id as well, this could be considered a more compact
 * that delivers the same functionality.
 */
typedef union lw_mutex_u {
    struct {
        lw_waiter_id_t lw_mutex_owner;
        lw_waiter_id_t lw_mutex_waitq; /* Reverse linked FIFO */
    };
    volatile lw_uint32_t lw_mutex_val;
    lw_uint16_t lw_mutex_ow[2];
} __attribute__ ((__packed__)) lw_mutex_t;

#define LW_MUTEX_INITIALIZER  { .lw_mutex_ow = { LW_WAITER_ID_MAX, LW_WAITER_ID_MAX} }

extern void
lw_mutex_init(LW_INOUT lw_mutex_t *lw_mutex);

extern void
lw_mutex_destroy(LW_INOUT lw_mutex_t *lw_mutex);

extern lw_int32_t
lw_mutex_trylock(LW_INOUT lw_mutex_t *lw_mutex);

extern void
lw_mutex_lock(LW_INOUT lw_mutex_t *lw_mutex);

/* Lock a mutex if not currently owner. Return TRUE if caller was not owner
 * already. FALSE otherwise. In either case, lock is held on return.
 */
extern lw_bool_t
lw_mutex_lock_if_not_held(LW_INOUT lw_mutex_t *lw_mutex);

/*
 * Chek if lw_mutex is held by the caller and then unlock.
 */
extern void
lw_mutex_unlock_if_held(LW_INOUT lw_mutex_t *lw_mutex);

/*
 * Unlock an lw_mutex. If there is a waiter, hand over the lock to the oldest
 * waiter.
 */
void lw_mutex_unlock(LW_INOUT lw_mutex_t *lw_mutex);


#ifdef LW_DEBUG
extern void
lw_mutex_assert_locked(lw_mutex_t *lw_mutex);

extern void
lw_mutex_assert_not_locked(lw_mutex_t *lw_mutex);
#else
#define lw_mutex_assert_locked(lwm)      LW_UNUSED_PARAMETER(lwm) /* Do Nothing */
#define lw_mutex_assert_not_locked(lwm)  LW_UNUSED_PARAMETER(lwm) /* Do Nothing */
#endif

#endif
