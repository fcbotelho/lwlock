#ifndef _LW_THREAD_H_
#define _LW_THREAD_H_

#include "lw_types.h"
#include "lw_sync_log.h"
#include <pthread.h>

typedef struct lw_thread_id_s *lw_thread_t;
typedef void *(*lw_thread_run_func_t)(void *);

extern int 
lw_thread_create(lw_thread_t *thread,
                 pthread_attr_t *attr,
                 lw_thread_run_func_t start_func,
                 void *arg,
                 char const *name);

extern int 
lw_thread_create_detached(lw_thread_t *thread,
                          lw_thread_run_func_t start_func,
                          void *arg,
                          char const *name);
/** 
 * Get pthread_id underlying the given lw_thread.
 */
extern pthread_t
lw_thread_get_ptid(lw_thread_t tid);

/**
 * exit the calling thread
 */
#define lw_thread_exit(arg)         pthread_exit(arg)

extern void
lw_thread_join(lw_thread_t tid, void **retval);

extern void
lw_thread_cancel(lw_thread_t tid);

extern void
lw_thread_detach(lw_thread_t tid);

extern lw_thread_t lw_thread_self(void);

extern void
lw_thread_system_init(lw_bool_t track_sync_events);

extern void
lw_thread_system_shutdown(void);

extern lw_sync_log_line_t *
lw_thread_sync_log_next_line(void);

#endif /* _DD_THREAD_H_ */
