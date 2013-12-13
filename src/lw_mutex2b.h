#ifndef __LW_MUTEX2B_H__
#define __LW_MUTEX2B_H__

#include "lw_types.h"
#include "lw_waiter.h"
#include "lw_lock_stats.h"


/* Now for 2byte version of dd_lwmutex_t called dd_lwmutex2b_t. */
typedef dd_thread_wait_id_t dd_lwmutex2b_t;

#define DD_LWMUTEX2B_INITIALIZER  DD_THREAD_WAIT_ID_MAX

static inline void
dd_lwmutex2b_init(LW_INOUT dd_lwmutex2b_t *lwmutex2b)
{
    *lwmutex2b = DD_THREAD_WAIT_ID_MAX;
}

static inline void
dd_lwmutex2b_destroy(LW_INOUT dd_lwmutex2b_t *lwmutex2b)
{
    lw_verify(*lwmutex2b == DD_THREAD_WAIT_ID_MAX);
}

extern int dd_lwmutex2b_trylock(LW_INOUT dd_lwmutex2b_t *lwmutex2b);

/* Lock an lwmutex2b. */
extern void
dd_lwmutex2b_lock(LW_INOUT dd_lwmutex2b_t *lwmutex2b,
                  LW_INOUT dd_lwlock_stats_t *lwlock_stats);

/* Unlock an lwmutex2b. If there is a waiter, hand over the lock to the oldest waiter. */
extern void
_dd_lwmutex2b_unlock(dd_lwmutex2b_t *lwmutex2b, lw_bool_t trace);
#define dd_lwmutex2b_unlock(l)      _dd_lwmutex2b_unlock(l, TRUE)

#ifdef LW_DEBUG
extern void dd_assert_lwmutex2b_locked(dd_lwmutex2b_t *lwmutex2b);
extern void dd_assert_lwmutex2b_not_locked(dd_lwmutex2b_t *lwmutex2b);
#else
#define dd_assert_lwmutex2b_locked(lwm)   UNUSED_PARAMETER(lwm) /* Do Nothing */
#define dd_assert_lwmutex2b_not_locked(lwm)   UNUSED_PARAMETER(lwm) /* Do Nothing */
#endif

#endif
