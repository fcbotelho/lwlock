#ifndef __LW_RWLOCK_H__
#define __LW_RWLOCK_H__

#include "lw_types.h"
#include "lw_waiter.h"
#include "lw_lock_stats.h"


/**
 * Lightweight read/write locks
 *
 * Has the same functionality as dd_rwlock (which is based on pthread_rwlock),
 * with smaller space (4-bytes) and faster non-contention performance (only
 * one 32-bit cmpxchg op).  However, performance under contention is worse
 * than dd_rwlock. This lock is also completely fair, whereas dd_rwlock
 * sacrifices fairness for higher reader throughput.
 */

typedef union dd_lwlock_u {
    struct {
        union {
            struct {
                dd_uint16_t unfair  : 1;
                dd_uint16_t wlocked : 1;
                dd_uint16_t readers : 14;
            };
            struct {
                dd_uint16_t flags : 1;
                dd_uint16_t locked : 15;
            };
        };
        dd_thread_wait_id_t wait_id;
    };
    volatile dd_uint32_t val;
    struct {
        dd_uint16_t flags : 1;
        dd_uint16_t locked : 15;
        dd_thread_wait_id_t wait_id;
    } init;
} __attribute__ ((__packed__)) dd_lwlock_t;

typedef enum {
    DD_LWLOCK_SHARED        = 0x0,
    DD_LWLOCK_WAIT          = 0x0, /* Blocking lock. */
    DD_LWLOCK_WAIT_INLINE   = 0x0, /* Wait right away on blocking lock */
    DD_LWLOCK_EXCLUSIVE     = 0x1,
    DD_LWLOCK_UPGRADE       = 0x2,
    DD_LWLOCK_NOWAIT        = 0x4, /* Try lock. WAIT_INLINE/DEFERRED meaningless if set. */
    DD_LWLOCK_WAIT_DEFERRED = 0x8, /* Enqueue waiter but don't wait right away. */
} dd_lwlock_attempt_t;

typedef enum dd_lwlock_flags_e {
    DD_LWLOCK_FAIR = 0x00,
    DD_LWLOCK_UNFAIR = 0x01,
    DD_LWLOCK_DEFAULT = DD_LWLOCK_FAIR,
} dd_lwlock_flags_t;

#define DD_LWLOCK_INIT(_f)  { .init = { .flags = _f, .locked = 0, .wait_id = DD_THREAD_WAIT_ID_MAX } }
#define DD_LWLOCK_INITIALIZER  DD_LWLOCK_INIT(DD_LWLOCK_DEFAULT)

int
dd_lwlock_lock(LW_INOUT dd_lwlock_t *lwlock,
               LW_IN dd_lwlock_attempt_t type,
               LW_INOUT dd_thread_wait_t *waiter,
               LW_INOUT dd_lwlock_stats_t *lwlock_stats);

void
dd_lwlock_unlock(LW_INOUT dd_lwlock_t *lwlock,
                 LW_IN lw_bool_t exclusive,
                 LW_INOUT dd_lwlock_stats_t *lwlock_stats);

/* Downgrade a lwlock from writer lock to reader lock */
extern void
dd_lwlock_downgrade(LW_INOUT dd_lwlock_t *lwlock,
                    LW_INOUT dd_lwlock_stats_t *stats);

extern int
dd_lwlock_upgrade(LW_INOUT dd_lwlock_t *lwlock,
                  LW_INOUT dd_lwlock_stats_t *stats);

static inline lw_bool_t
dd_lwlock_has_waiters(LW_INOUT dd_lwlock_t *lwlock)
{
    dd_lwlock_t old;
    old.val = lwlock->val;
    return (old.wait_id != DD_THREAD_WAIT_ID_MAX);
}

#define dd_lwlock_async_done(waiter)    dd_thread_wait_wakeup_pending(waiter)

void dd_lwlock_contention_wait(LW_INOUT dd_lwlock_t *lwlock,
                               dd_lwlock_attempt_t type,
                               LW_INOUT dd_thread_wait_t *waiter,
                               LW_INOUT dd_lwlock_stats_t *lwlock_stats);

void dd_lwlock_init(LW_INOUT dd_lwlock_t *lwlock, LW_IN dd_lwlock_flags_t flags);
void dd_lwlock_destroy(LW_INOUT dd_lwlock_t *lwlock);
void dd_lwlock_stats_init(LW_INOUT dd_lwlock_stats_t *lwlock_stats,
                          LW_IN char *name);

#define DD_LWLOCK_STATS_TRACE_ON(stats)         ((stats)->trace_history = 1)
#define DD_LWLOCK_STATS_TRACE_OFF(stats)        ((stats)->trace_history = 0)

lw_bool_t
dd_lwlock_stats_indicate_contention(LW_IN dd_lwlock_stats_t *lwlock_stats);
void dd_lwlock_stats_reset(LW_INOUT dd_lwlock_stats_t *lwlock_stats);
void dd_lwlock_stats_str(LW_IN dd_lwlock_stats_t *lwlock_stats,
                         char *buf,
                         size_t size,
                         size_t *len);

#define dd_lwlock_rdlock(_lwlock) \
        lw_verify(dd_lwlock_lock(_lwlock, DD_LWLOCK_SHARED | DD_LWLOCK_WAIT, NULL, NULL) == 0)
#define dd_lwlock_tryrdlock(_lwlock) \
        dd_lwlock_lock(_lwlock, DD_LWLOCK_SHARED | DD_LWLOCK_NOWAIT, NULL, NULL)
#define dd_lwlock_wrlock(_lwlock) \
        lw_verify(dd_lwlock_lock(_lwlock, DD_LWLOCK_EXCLUSIVE | DD_LWLOCK_WAIT, NULL, NULL) == 0)
#define dd_lwlock_trywrlock(_lwlock) \
        dd_lwlock_lock(_lwlock, DD_LWLOCK_EXCLUSIVE | DD_LWLOCK_NOWAIT, NULL, NULL)
#define dd_lwlock_rdunlock(_lwlock) dd_lwlock_unlock(_lwlock, FALSE, NULL)
#define dd_lwlock_wrunlock(_lwlock) dd_lwlock_unlock(_lwlock, TRUE, NULL)

#define dd_lwlock_rdlock_with_stats(_lwlock, _stats) \
        lw_verify(dd_lwlock_lock(_lwlock, DD_LWLOCK_SHARED | DD_LWLOCK_WAIT, NULL, _stats) == 0)
#define dd_lwlock_wrlock_with_stats(_lwlock, _stats) \
        lw_verify(dd_lwlock_lock(_lwlock, DD_LWLOCK_EXCLUSIVE | DD_LWLOCK_WAIT, NULL, _stats) == 0)
#define dd_lwlock_rdunlock_with_stats(_lwlock, _stats) dd_lwlock_unlock(_lwlock, FALSE, _stats)
#define dd_lwlock_wrunlock_with_stats(_lwlock, _stats) dd_lwlock_unlock(_lwlock, TRUE, _stats)

#ifdef DD_DEBUG
static inline void
dd_assert_lwlock_rdlocked(dd_lwlock_t *lock)
{
    lw_assert(lock->readers != 0);
}
#else
#define dd_assert_lwlock_rdlocked(lock) UNUSED_PARAMETER(lock)
#endif

#ifdef DD_DEBUG
static inline void
dd_assert_lwlock_wrlocked(dd_lwlock_t *lock)
{
    lw_assert(lock->wlocked != 0);
}
#else
#define dd_assert_lwlock_wrlocked(lock) UNUSED_PARAMETER(lock)
#endif

#ifdef DD_DEBUG
static inline void
dd_assert_lwlock_locked(dd_lwlock_t *lock)
{
    lw_assert((lock->readers != 0) || (lock->wlocked != 0));
    lw_assert(dd_lwlock_trywrlock(lock) != 0);
}
#else
#define dd_assert_lwlock_locked(lock)  UNUSED_PARAMETER(lock)
#endif

#ifdef DD_DEBUG
static inline void
dd_assert_lwlock_unlocked(dd_lwlock_t *lock)
{
    lw_assert(lock->locked == 0);
}
#else
#define dd_assert_lwlock_unlocked(lock) UNUSED_PARAMETER(lock)
#endif

static inline lw_bool_t
dd_lwlock_wrlock_waiters(dd_lwlock_t *lock)
{
    return (lock->wait_id != DD_THREAD_WAIT_ID_MAX);
}

#endif
