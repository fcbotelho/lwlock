/***
 * Developed originally at EMC Corporation, this library is released under the
 * MPL 2.0 license.  Please refer to the MPL-2.0 file in the repository for its
 * full description or to http://www.mozilla.org/MPL/2.0/ for the online version.
 *
 * Before contributing to the project one needs to sign the committer agreement
 * available in the "committerAgreement" directory.
 */

#ifndef __LW_COND_VAR_H__
#define __LW_COND_VAR_H__

#include "lw_types.h"
#include "lw_waiter.h"
#include "lw_lock_common.h"
#include "lw_mutex2b.h"


/* Light-weight condition variables to go with lw_mutex or any other mutex. */
typedef struct lw_condvar_u {
    lw_mutex2b_t waitq_lock;
    lw_waiter_id_t waitq;
} ALIGNED_PACKED(4) lw_condvar_t;

#define DD_LWCONDVAR_INITIALIZER    { LW_MUTEX2B_INITIALIZER, LW_WAITER_ID_MAX }

static inline void
lw_condvar_init(LW_INOUT lw_condvar_t *lwcondvar)
{
    lw_mutex2b_init(&lwcondvar->waitq_lock);
    lwcondvar->waitq = LW_WAITER_ID_MAX;
}

static inline void
lw_condvar_destroy(LW_INOUT lw_condvar_t *lwcondvar)
{
    lw_verify(lwcondvar->waitq == LW_WAITER_ID_MAX);
    lw_mutex2b_destroy(&lwcondvar->waitq_lock);
}

extern void
lw_condvar_wait(LW_INOUT lw_condvar_t *lwcondvar,
                LW_INOUT void *_mutex,
                LW_IN lw_lock_type_t type);

extern int
lw_condvar_timedwait(LW_INOUT lw_condvar_t *lwcondvar,
                     LW_INOUT void *_mutex,
                     LW_IN lw_lock_type_t type,
                     LW_IN struct timespec *abstime);

extern void
lw_condvar_signal(LW_INOUT lw_condvar_t *lwcondvar);

extern void
lw_condvar_broadcast(LW_INOUT lw_condvar_t *lwcondvar);

#endif
