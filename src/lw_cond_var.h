#ifndef __LW_COND_VAR_H__
#define __LW_COND_VAR_H__

#include "lw_types.h"
#include "lw_waiter.h"
#include "lw_lock_stats.h"
#include "lw_lock_common.h"
#include "lw_mutex2b.h"


/* Light-weight condition variables to go with lw_mutex or any other mutex. */
typedef struct lw_condvar_u {
    lw_mutex2b_t cmutex;
    lw_waiter_id_t waiter_id_list;
} lw_condvar_t;

#define DD_LWCONDVAR_INITIALIZER    { LW_MUTEX2B_INITIALIZER, LW_WAITER_ID_MAX }

static inline void
lw_condvar_init(LW_INOUT lw_condvar_t *lwcondvar)
{
    lw_mutex2b_init(&lwcondvar->cmutex);
    lwcondvar->waiter_id_list = LW_WAITER_ID_MAX;
}

static inline void
lw_condvar_destroy(LW_INOUT lw_condvar_t *lwcondvar)
{
    lw_verify(lwcondvar->waiter_id_list == LW_WAITER_ID_MAX);
    lw_mutex2b_destroy(&lwcondvar->cmutex);
}

extern void
lw_condvar_wait(LW_INOUT void *_mutex,
                LW_IN lw_lock_type_t type,
                LW_INOUT lw_lock_stats_t *stats,
                LW_INOUT lw_condvar_t *lwcondvar);

extern int
lw_condvar_timedwait(LW_INOUT void *_mutex,
                     LW_IN lw_lock_type_t type,
                     LW_INOUT lw_lock_stats_t *stats,
                     LW_INOUT lw_condvar_t *lwcondvar,
                     LW_IN struct timespec *abstime);

extern void
lw_condvar_signal(LW_INOUT lw_condvar_t *lwcondvar);

extern void
lw_condvar_broadcast(LW_INOUT lw_condvar_t *lwcondvar);

#endif
