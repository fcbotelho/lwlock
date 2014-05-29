/***
 * Developed originally at EMC Corporation, this library is released under the
 * MPL 2.0 license.  Please refer to the MPL-2.0 file in the repository for its
 * full description or to http://www.mozilla.org/MPL/2.0/ for the online version.
 *
 * Before contributing to the project one needs to sign the committer agreement
 * available in the "committerAgreement" directory.
 */

#ifndef __LW_LOCK_COMMON_H__
#define __LW_LOCK_COMMON_H__

#include "lw_waiter.h"
#include "lw_mutex.h"
#include "lw_mutex2b.h"
#include "lw_rwlock.h"
#include "lw_bitlock.h"
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
    LW_LOCK_TYPE_BITLOCK32,
    LW_LOCK_TYPE_BITLOCK64,
    LW_LOCK_TYPE_NONE /* a no-op lock that simulates presence of race */
} lw_lock_type_t;

static inline const char *
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
        case LW_LOCK_TYPE_BITLOCK32:
            return "LW_LOCK_TYPE_BITLOCK32";
        case LW_LOCK_TYPE_BITLOCK64:
            return "LW_LOCK_TYPE_BITLOCK64";
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
                            LW_INOUT lw_waiter_t *waiter)
{
    switch(type) {
        case LW_LOCK_TYPE_PMUTEX:
            pthread_mutex_lock(lock);
            break;
        case LW_LOCK_TYPE_LWMUTEX:
            lw_mutex_lock(lock);
            break;
        case LW_LOCK_TYPE_LWMUTEX2B:
            lw_mutex2b_lock(lock);
            break;
        case LW_LOCK_TYPE_LWRWLOCK_RD:
            lw_verify(lw_rwlock_lock(lock, LW_RWLOCK_SHARED | LW_RWLOCK_WAIT, waiter) == 0);
            break;
        case LW_LOCK_TYPE_LWRWLOCK_WR:
            lw_verify(lw_rwlock_lock(lock, LW_RWLOCK_EXCLUSIVE | LW_RWLOCK_WAIT, waiter) == 0);
            break;
        case LW_LOCK_TYPE_BITLOCK32: {
            lw_bitlock32_spec_t *spec = (lw_bitlock32_spec_t *)lock;
            lw_bitlock32_lock(spec->lock, spec->lock_mask, spec->wait_mask);
            break;
        }
        case LW_LOCK_TYPE_BITLOCK64: {
            lw_bitlock64_spec_t *spec = (lw_bitlock64_spec_t *)lock;
            lw_bitlock64_lock(spec->lock, spec->lock_mask, spec->wait_mask);
            break;
        }
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
                         LW_IN lw_lock_type_t type)
{
    switch(type) {
        case LW_LOCK_TYPE_PMUTEX:
            pthread_mutex_unlock(lock);
            break;
        case LW_LOCK_TYPE_LWMUTEX:
            lw_mutex_unlock(lock);
            break;
        case LW_LOCK_TYPE_LWMUTEX2B:
            lw_mutex2b_unlock(lock);
            break;
        case LW_LOCK_TYPE_LWRWLOCK_RD:
             lw_rwlock_unlock(lock, FALSE);
            break;
        case LW_LOCK_TYPE_LWRWLOCK_WR:
            lw_rwlock_unlock(lock, TRUE);
            break;
        case LW_LOCK_TYPE_BITLOCK32: {
            lw_bitlock32_spec_t *spec = (lw_bitlock32_spec_t *)lock;
            lw_bitlock32_unlock(spec->lock, spec->lock_mask, spec->wait_mask);
            break;
        }
        case LW_LOCK_TYPE_BITLOCK64: {
            lw_bitlock64_spec_t *spec = (lw_bitlock64_spec_t *)lock;
            lw_bitlock64_unlock(spec->lock, spec->lock_mask, spec->wait_mask);
            break;
        }
        case LW_LOCK_TYPE_NONE:
            /* no op */
            break;
        default:
            fprintf(stderr, "%s: unknown lock type\n", __func__);
            lw_verify(FALSE);
    }
}

#endif
