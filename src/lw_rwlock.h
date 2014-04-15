/***
 * Developed originally at EMC Corporation, this library is released under the
 * MPL 2.0 license.  Please refer to the MPL-2.0 file in the repository for its
 * full description or to http://www.mozilla.org/MPL/2.0/ for the online version.
 *
 * Before contributing to the project one needs to sign the committer agreement
 * available in the "committerAgreement" directory.
 */

#ifndef __LW_RWLOCK_H__
#define __LW_RWLOCK_H__

#include "lw_types.h"
#include "lw_waiter.h"

/**
 * Lightweight read/write locks
 *
 * Has the same functionality as pthread_rwlock, with smaller space (4-bytes)
 * and faster non-contention performance (only one 32-bit cmpxchg op).  However,
 * performance under contention is worse than pthread_rwlock. This lock is by default
 * completely fair, whereas pthread_rwlock sacrifices fairness for higher reader
 * throughput.
 */

typedef union lw_rwlock_u {
    struct {
        union {
            struct {
                lw_uint16_t unfair  : 1;
                lw_uint16_t wlocked : 1;
                lw_uint16_t readers : 14;
            };
            struct {
                lw_uint16_t flags : 1;
                lw_uint16_t locked : 15;
            };
        };
        lw_waiter_id_t waitq;
    };
    volatile lw_uint32_t val;
    struct {
        lw_uint16_t flags : 1;
        lw_uint16_t locked : 15;
        lw_waiter_id_t waitq;
    } lw_rwlock_init;
} ALIGNED_PACKED(4) lw_rwlock_t;

typedef enum {
    LW_RWLOCK_SHARED        = 0x0,
    LW_RWLOCK_WAIT          = 0x0, /* Blocking lock. */
    LW_RWLOCK_WAIT_INLINE   = 0x0, /* Wait right away on blocking lock */
    LW_RWLOCK_EXCLUSIVE     = 0x1,
    LW_RWLOCK_UPGRADE       = 0x2,
    LW_RWLOCK_NOWAIT        = 0x4, /* Try lock. WAIT_INLINE/DEFERRED meaningless if set. */
    LW_RWLOCK_WAIT_DEFERRED = 0x8, /* Enqueue waiter but don't wait right away. */
} lw_rwlock_attempt_t;

typedef enum lw_rwlock_flags_e {
    LW_RWLOCK_FAIR = 0x00,
    LW_RWLOCK_UNFAIR = 0x01,
    LW_RWLOCK_DEFAULT = LW_RWLOCK_FAIR,
} lw_rwlock_flags_t;

#define LW_RWLOCK_INIT(_f)  \
{ .lw_rwlock_init = { .lw_rwlock_flags = _f, \
                      .lw_rwlock_locked = 0, \
                      .waitq = LW_WAITER_ID_MAX } }
#define LW_RWLOCK_INITIALIZER  LW_RWLOCK_INIT(LW_RWLOCK_DEFAULT)

int
lw_rwlock_lock(LW_INOUT lw_rwlock_t *rwlock,
               LW_IN lw_rwlock_attempt_t type,
               LW_INOUT lw_waiter_t *waiter);

void
lw_rwlock_unlock(LW_INOUT lw_rwlock_t *rwlock,
                 LW_IN lw_bool_t exclusive);

/* Downgrade a rwlock from writer lock to reader lock */
extern void
lw_rwlock_downgrade(LW_INOUT lw_rwlock_t *rwlock);

extern int
lw_rwlock_upgrade(LW_INOUT lw_rwlock_t *rwlock);

static inline lw_bool_t
lw_rwlock_has_waiters(LW_INOUT lw_rwlock_t *rwlock)
{
    lw_rwlock_t old;
    old.val = rwlock->val;
    return (old.waitq != LW_WAITER_ID_MAX);
}

#define lw_rwlock_async_done(waiter)    lw_thread_wakeup_pending(waiter)

void lw_rwlock_contention_wait(LW_INOUT lw_rwlock_t *rwlock,
                               LW_IN lw_rwlock_attempt_t type,
                               LW_INOUT lw_waiter_t *waiter);

void lw_rwlock_init(LW_INOUT lw_rwlock_t *rwlock, LW_IN lw_rwlock_flags_t flags);
void lw_rwlock_destroy(LW_INOUT lw_rwlock_t *rwlock);

static inline void
lw_rwlock_rdlock(LW_INOUT lw_rwlock_t *rwlock)
{
        lw_verify(lw_rwlock_lock(rwlock, LW_RWLOCK_SHARED | LW_RWLOCK_WAIT, NULL) == 0);
}

static inline int
lw_rwlock_tryrdlock(LW_INOUT lw_rwlock_t *rwlock)
{
        return lw_rwlock_lock(rwlock, LW_RWLOCK_SHARED | LW_RWLOCK_NOWAIT, NULL);
}

static inline void
lw_rwlock_wrlock(LW_INOUT lw_rwlock_t *rwlock)
{
        lw_verify(lw_rwlock_lock(rwlock, LW_RWLOCK_EXCLUSIVE | LW_RWLOCK_WAIT, NULL) == 0);
}

static inline int
lw_rwlock_trywrlock(LW_INOUT lw_rwlock_t *rwlock)
{
        return lw_rwlock_lock(rwlock, LW_RWLOCK_EXCLUSIVE | LW_RWLOCK_NOWAIT, NULL);
}

static inline void
lw_rwlock_rdunlock(LW_INOUT lw_rwlock_t *rwlock)
{
    lw_rwlock_unlock(rwlock, FALSE);
}

static inline void
lw_rwlock_wrunlock(LW_INOUT lw_rwlock_t *rwlock)
{
    lw_rwlock_unlock(rwlock, TRUE);
}

#ifdef LW_DEBUG
static inline void
lw_rwlock_assert_rdlocked(LW_IN lw_rwlock_t *rwlock)
{
    lw_assert(rwlock->lw_rwlock_readers != 0);
}
#else
#define lw_rwlock_assert_rdlocked(rwlock) LW_UNUSED_PARAMETER(rwlock)
#endif

#ifdef LW_DEBUG
static inline void
lw_rwlock_assert_wrlocked(LW_IN lw_rwlock_t *rwlock)
{
    lw_assert(rwlock->lw_rwlock_wlocked != 0);
}
#else
#define lw_rwlock_assert_wrlocked(rwlock) LW_UNUSED_PARAMETER(rwlock)
#endif

#ifdef LW_DEBUG
static inline void
lw_rwlock_assert_locked(LW_IN lw_rwlock_t *rwlock)
{
    lw_assert((rwlock->lw_rwlock_readers != 0) || (rwlock->lw_rwlock_wlocked != 0));
    lw_assert(lw_rwlock_trywrlock(rwlock) != 0);
}
#else
#define lw_rwlock_assert_locked(rwlock)  UNUSED_PARAMETER(rwlock)
#endif

#ifdef LW_DEBUG
static inline void
lw_rwlock_assert_unlocked(LW_IN lw_rwlock_t *rwlock)
{
    lw_assert(lock->lw_rwlock_locked == 0);
}
#else
#define lw_rwlock_assert_unlocked(rwlock) UNUSED_PARAMETER(rwlock)
#endif

static inline lw_bool_t
lw_rwlock_wrlock_waiters(LW_IN lw_rwlock_t *rwlock)
{
    return (rwlock->waitq != LW_WAITER_ID_MAX);
}

#endif
