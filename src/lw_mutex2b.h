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

/* Lock an lw_mutex2b. */
extern void
lw_mutex2b_lock(LW_INOUT lw_mutex2b_t *lw_mutex2b,
                  LW_INOUT lw_lock_stats_t *lw_lock_stats);

/*
 * Unlock an lwmutex2b. If there is a waiter, hand over the lock 
 * to the oldest waiter. 
 * */
extern void
lw_mutex2b_unlock(LW_INOUT lw_mutex2b_t *lw_mutex2b, 
                    LW_IN lw_bool_t trace);

#ifdef LW_DEBUG
extern void lw_mutex2b_assert_locked(LW_INOUT lw_mutex2b_t *lw_mutex2b);
extern void lw_mutex2b_assert_not_locked(lw_mutex2b_t *lw_mutex2b);
#else
#define lw_mutex2b_assert_locked(lwm) LW_UNUSED_PARAMETER(lwm) /* Do Nothing */
#define lw_mutex2b_assert_not_locked(lwm) LW_UNUSED_PARAMETER(lwm) /* Do Nothing */
#endif

#endif
