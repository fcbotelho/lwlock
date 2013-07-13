#ifndef __LW_COND_VAR_H__
#define __LW_COND_VAR_H__

#include "lw_types.h"
#include "lw_waiter.h"
#include "lw_lock_stats.h"


/* Light-weight condition variables to go with lwmutex or any other mutex. */
typedef struct dd_lwcondvar_u {
    dd_lwmutex2b_t cmutex;
    dd_thread_wait_id_t waiter_id_list;
} dd_lwcondvar_t;

#define DD_LWCONDVAR_INITIALIZER    { DD_LWMUTEX2B_INITIALIZER, DD_THREAD_WAIT_ID_MAX }

static inline void
dd_lwcondvar_init(LW_INOUT dd_lwcondvar_t *lwcondvar)
{
    dd_lwmutex2b_init(&lwcondvar->cmutex);
    lwcondvar->waiter_id_list = DD_THREAD_WAIT_ID_MAX;
}

static inline void
dd_lwcondvar_destroy(LW_INOUT dd_lwcondvar_t *lwcondvar)
{
    lw_verify(lwcondvar->waiter_id_list == DD_THREAD_WAIT_ID_MAX);
    dd_lwmutex2b_destroy(&lwcondvar->cmutex);
}

typedef enum {
    DD_LWCONDVAR_WAIT_LOCK_TYPE_PMUTEX,
    DD_LWCONDVAR_WAIT_LOCK_TYPE_PRWLOCK_RD,
    DD_LWCONDVAR_WAIT_LOCK_TYPE_PRWLOCK_WR,
    DD_LWCONDVAR_WAIT_LOCK_TYPE_LWLOCK_RD,
    DD_LWCONDVAR_WAIT_LOCK_TYPE_LWLOCK_WR,
    DD_LWCONDVAR_WAIT_LOCK_TYPE_LWMUTEX,
    DD_LWCONDVAR_WAIT_LOCK_TYPE_LWMUTEX2B,
    DD_LWCONDVAR_WAIT_LOCK_TYPE_SPINLOCK,
} dd_lwcondvar_mutex_type_t;

extern void
dd_lwcondvar_wait(LW_INOUT void *_mutex,
                  LW_IN dd_lwcondvar_mutex_type_t type,
                  LW_INOUT dd_lwlock_stats_t *stats,
                  LW_INOUT dd_lwcondvar_t *lwcondvar);

extern int
dd_lwcondvar_timedwait(LW_INOUT void *_mutex,
                       LW_IN dd_lwcondvar_mutex_type_t type,
                       LW_INOUT dd_lwlock_stats_t *stats,
                       LW_INOUT dd_lwcondvar_t *lwcondvar,
                       LW_IN struct timespec *abstime);

extern void
dd_lwcondvar_signal(LW_INOUT dd_lwcondvar_t *lwcondvar);

extern void
dd_lwcondvar_broadcast(LW_INOUT dd_lwcondvar_t *lwcondvar);

#endif
