#ifndef __LW_SYNC_LOG_H__
#define __LW_SYNC_LOG_H__

#include "lw_magic.h"
#include "lw_thread.h"

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
    /* NULL if no name associated with primitive */
    const char *lw_sync_logline_name;

    /* Pointer to mutex/condvar/counter etc */
    const void *lw_sync_logline_lock_ptr;

    /* Start tsc of event recorded. */
    lw_uint64_t lw_sync_logline_start_tsc;

    /* End tsc */
    lw_uint64_t lw_sync_logline_end_tsc;

    /* Type of event: lock/unlock etc. */
    lw_sync_event_type_t lw_sync_logline_event_id;

    /* type of primitive being acquired. */
    lw_sync_type_t lw_sync_logline_primitive_type;

    /* Pid of current owner. 0 if pid can't be determined. */
    lw_uint32_t lw_sync_logline_pid_of_contending_owner;

    /* Tid of current owner. NULL if it can't be determined */
    lw_thread_t lw_sync_logline_tid_of_contending_owner;

    /* Any sync event specific data to keep */
    lw_uint64_t lw_sync_logline_specific_data[4];
} lw_sync_logline_t;

#define LW_MAX_SYNC_LOGLINES    (4096)

#define  LW_SYNC_LOG_MAGIC   LW_MAGIC(0x5106)

typedef struct {
    lw_magic_t lw_sync_log_magic;
    lw_uint32_t lw_sync_log_next_line;
    lw_sync_logline_t lw_sync_log_lines[LW_MAX_SYNC_LOGLINES];
} lw_sync_log_t;

/* Functions to init/shutdown the entire sync log API */
extern void
lw_sync_log_init(void);

extern void
lw_sync_log_shutdown(void);

/* Functions to handle sync log for each thread */
extern lw_sync_log_t *
lw_sync_log_register(void);

extern void 
lw_sync_log_unregister(void);

extern lw_sync_log_t *
lw_sync_log_get(void);

extern lw_sync_logline_t *
lw_next_sync_log_line(void);

#endif
