#ifndef __LW_MUTEX2B_H__
#define __LW_MUTEX2B_H__

#include "lw_types.h"
#include "lw_lock_stats.h"

/*
 * Lightweight mutex with 2 bytes.
 *
 * Has the same functionality as a lw_mutex_t but cannot track the owner
 * of the mutex.
 */
typedef lw_waiter_id_t lw_mutex2b_t;

#define LW_MUTEX2B_INITIALIZER  LW_WAITER_ID_MAX

extern void
lw_mutex2b_init(LW_INOUT lw_mutex2b_t *lw_mutex2b);

extern void
lw_mutex2b_destroy(LW_INOUT lw_mutex2b_t *lw_mutex2b);

extern lw_int32_t
lw_mutex2b_trylock(LW_INOUT lw_mutex2b_t *lw_mutex2b);

/* Lock an lwmutex2b. */
extern void
dd_lwmutex2b_lock(LW_INOUT dd_lwmutex2b_t *lwmutex2b,
                  LW_INOUT dd_lwlock_stats_t *lwlock_stats);

/* Unlock an lwmutex2b. If there is a waiter, hand over the lock to the oldest waiter. */
extern void
_dd_lwmutex2b_unlock(dd_lwmutex2b_t *lwmutex2b, lw_bool_t trace);
#define dd_lwmutex2b_unlock(l)      _dd_lwmutex2b_unlock(l, TRUE)

#ifdef DD_DEBUG
extern void dd_assert_lwmutex2b_locked(dd_lwmutex2b_t *lwmutex2b);
extern void dd_assert_lwmutex2b_not_locked(dd_lwmutex2b_t *lwmutex2b);
#else
#define dd_assert_lwmutex2b_locked(lwm)   UNUSED_PARAMETER(lwm) /* Do Nothing */
#define dd_assert_lwmutex2b_not_locked(lwm)   UNUSED_PARAMETER(lwm) /* Do Nothing */
#endif

#endif
