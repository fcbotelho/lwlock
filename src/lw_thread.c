#include "lw_thread.h"
#include "lw_sync_log.h"
#include "lw_waiter.h"
#include <pthread.h>

struct lw_thread_s {
    pthread_t      lw_thread_pthread_id;
    lw_uint32_t    lw_thread_thread_pid;
    lw_bool_t      lw_thread_detached;
    lw_waiter_t    *lw_thread_waiter;
    // TODO: port dd_thread_err_stack_t and operations
    // lw_err_stack_t *lw_thread_err_stack;
    const char     *lw_thread_name;
    lw_int32_t     *lw_thread_errno_ptr;
    lw_sync_log_t  *lw_thread_sync_log;
};


lw_thread_t lw_thread_self(void)
{
    return NULL;
}
