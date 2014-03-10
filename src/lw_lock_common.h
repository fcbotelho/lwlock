#ifndef __LW_LOCK_COMMON_H__
#define __LW_LOCK_COMMON_H__

#include "lw_waiter.h"
#include "lw_mutex.h"
#include "lw_mutex2b.h"
#include <pthread.h>

/* This file implements a common set of APIs for lw lock primitives. 
 * This common set of API has two uses:
 *    1) write test program that works on different kinds of locks
 *       with minimum code duplication
 *    2) allow lw_condvar to work with different kinds of mutexes
 */

typedef enum {
    LW_LOCK_TYPE_PMUTEX = 0, /* pthread mutex */
    LW_LOCK_TYPE_LWMUTEX,
    LW_LOCK_TYPE_LWMUTEX2B,
    LW_LOCK_TYPE_LWRWLOCK_RD,
    LW_LOCK_TYPE_LWRWLOCK_WR,
    LW_LOCK_TYPE_NONE /* a no-op lock that simulates presence of race */
} lw_lock_type_t;

static const char *
lw_lock_common_lock_type_description(lw_lock_type_t type) {
    switch(type) {
        case LW_LOCK_TYPE_PMUTEX:
            return "LW_LOCK_TYPE_PMUTEX";
        case LW_LOCK_TYPE_LWMUTEX:
            return "LW_LOCK_TYPE_LWMUTEX";
        case LW_LOCK_TYPE_LWMUTEX2B:
            return "LW_LOCK_TYPE_LWMUTEX2B";
        case LW_LOCK_TYPE_LWRWLOCK_RD:
            return "LW_LOCK_TYPE_LWRWLOCK_RD";
        case LW_LOCK_TYPE_LWRWLOCK_WR:
            return "LW_LOCK_TYPE_LWRWLOCK_WR";
        case LW_LOCK_TYPE_NONE:
            return "LW_LOCK_TYPE_NONE";
        default:
            fprintf(stderr, "%s: unknown lock type\n", __func__);
            lw_verify(FALSE);
    }
}

static inline void 
lw_lock_common_acquire_lock(LW_INOUT void *lock,
                            LW_IN lw_lock_type_t type,
                            LW_IN lw_waiter_t *waiter,
                            LW_INOUT lw_lock_stats_t *stats)
{
    switch(type) {
        case LW_LOCK_TYPE_PMUTEX:
            pthread_mutex_lock(lock);
            break;
        case LW_LOCK_TYPE_LWMUTEX:
            lw_mutex_lock(lock, stats);
            break;
        case LW_LOCK_TYPE_LWMUTEX2B:
            lw_mutex2b_lock(lock, stats);
            break;
        case LW_LOCK_TYPE_LWRWLOCK_RD:
            /* TODO: uncomment the following line once lw_rwlock is implemented */
            // lw_verify(lw_rwlock_lock(lock, LW_LOCK_SHARED | LW_LOCK_WAIT, waiter, stats) == 0);
            break;
        case LW_LOCK_TYPE_LWRWLOCK_WR:
            /* TODO: uncomment the following line once lw_rwlock is implemented */
            // lw_verify(lw_rwlock_lock(lock, LW_LOCK_EXCLUSIVE | LW_LOCK_WAIT, waiter, stats) == 0);
            break;
        case LW_LOCK_TYPE_NONE:
            /* no op */
            break;
        default:
            fprintf(stderr, "%s: unknown lock type\n", __func__);
            lw_verify(FALSE);
    }
}

static inline void 
lw_lock_common_drop_lock(LW_INOUT void *lock,
                         LW_IN lw_lock_type_t type,
                         LW_INOUT lw_lock_stats_t *stats)
{
    switch(type) {
        case LW_LOCK_TYPE_PMUTEX:
            pthread_mutex_unlock(lock);
            break;
        case LW_LOCK_TYPE_LWMUTEX:
            lw_mutex_unlock(lock, TRUE);
            break;
        case LW_LOCK_TYPE_LWMUTEX2B:
            lw_mutex2b_unlock(lock, TRUE);
            break;
        case LW_LOCK_TYPE_LWRWLOCK_RD:
            /* TODO: uncomment the following line once lw_rwlock is implemented */
            // lw_rwlock_unlock(lock, FALSE, stats);
            break;
        case LW_LOCK_TYPE_LWRWLOCK_WR:
            /* TODO: uncomment the following line once lw_rwlock is implemented */
            // lw_rwlock_unlock(lock, TRUE, stats);
            break;
        case LW_LOCK_TYPE_NONE:
            /* no op */
            break;
        default:
            fprintf(stderr, "%s: unknown lock type\n", __func__);
            lw_verify(FALSE);
    }
}

#endif
