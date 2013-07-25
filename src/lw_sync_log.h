#ifndef __LW_SYNC_LOG_H__
#define __LW_SYNC_LOG_H__

#include "lw_types.h"

typedef enum {
    LW_SYNC_TYPE_UNKNOWN      = 0,
    LW_SYNC_TYPE_LWMUTEX      = 1,
    LW_SYNC_TYPE_LWMUTEX2B    = 2,
    LW_SYNC_TYPE_LWCONDVAR    = 3, 
    LW_SYNC_TYPE_LWLOCK_RD    = 4,     
    LW_SYNC_TYPE_LWLOCK_WR    = 5,
    LW_SYNC_TYPE_LWLOCK_DWNGR = 6,
    LW_SYNC_TYPE_LWLOCK_UPGR  = 7,
} lw_sync_type_t;


typedef enum {
    LW_SYNC_EVENT_TYPE_MUTEX_LOCK = 0,
    LW_SYNC_EVENT_TYPE_MUTEX_UNLOCK = 1,
    LW_SYNC_EVENT_TYPE_RWLOCK_RDLOCK = 2,
    LW_SYNC_EVENT_TYPE_RWLOCK_WRLOCK = 3,
    LW_SYNC_EVENT_TYPE_RWLOCK_UNLOCK = 4,
    LW_SYNC_EVENT_TYPE_COND_WAIT = 5,
    LW_SYNC_EVENT_TYPE_COND_TIMEDWAIT = 6,
    LW_SYNC_EVENT_TYPE_COND_SIGNAL = 7,
    LW_SYNC_EVENT_TYPE_COND_BROADCAST = 8,
    LW_SYNC_EVENT_TYPE_BARRIER_WAIT = 9,

} lw_sync_event_type_t;


typedef struct {
    const char *name;                   /* NULL if no name associated with primitive */
    const void *lock_ptr;               /* Pointer to mutex/condvar/counter etc */
    lw_uint64_t start_tsc;              /* Start tsc of event recorded. */
    lw_uint64_t end_tsc;                /* End tsc */
    lw_sync_event_type_t event_id;      /* Type of event: lock/unlock etc. */
    lw_sync_type_t primitive_type;     /* type of primitive being acquired. */
    dd_uint64_t specific_data[4];       /* Any sync event specific data to keep */
} lw_sync_logline_t;

#define LW_MAX_SYNC_LOGLINES    (4096)

#define  LW_SYNC_LOG_MAGIC   LW_MAGIC(0x5106)


typedef struct {
    lw_uint32_t magic;
    lw_uint32_t next_line;
    lw_sync_logline_t lines[LW_MAX_SYNC_LOGLINES];
} lw_sync_log_t;

/* Functions to init/shutdown the entire sync log API */
extern void
lw_sync_log_init(void);

extern void
lw_sync_log_shutdown(void);


/* Functions to handle sync log for each thread */
extern void
lw_sync_log_register(void);

extern void 
lw_sync_log_unregister(void);

extern lw_sync_log_t *
lw_sync_log_get(void);

extern lw_sync_logline_t *
lw_next_sync_log_line(void);




#endif
