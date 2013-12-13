#include "lw_thread.h"
#include "lw_waiter.h"
#include "lw_sync_log.h"

#include <errno.h>

struct lw_thread_id_s {
    pthread_t      lw_tid_pthread_id;
    // Removing lw_tid_thread_pid since this needs
    // system specific calls and does not look like
    // this is necessary
    // lw_uint32_t    lw_tid_thread_pid; 
    lw_bool_t      lw_tid_detached;
    lw_waiter_t    *lw_tid_waiter;
    // TODO: port dd_thread_err_stack_t and operations
    // lw_err_stack_t *lw_tid_err_stack;
    const char     *lw_tid_name;
    lw_int32_t     *lw_tid_errno_ptr;
    lw_sync_log_t  *lw_tid_sync_log;
};

typedef struct {
    lw_thread_run_func_t        lw_ta_start_func;
    void                        *lw_ta_arg;
    char const                  *lw_ta_name;
    lw_thread_t                 lw_ta_tid;
} _lw_thread_arg_t;


static pthread_key_t lw_thread_id_key;
static lw_bool_t lw_thread_initialized; // TRUE once lw_thread_system_init() is called
static lw_bool_t lw_thread_track_sync_events;

static lw_thread_t
lw_thread_id_alloc(lw_bool_t is_detached)
{
    lw_thread_t tid = malloc(sizeof(struct lw_thread_id_s));
    tid->lw_tid_pthread_id = pthread_self();
    tid->lw_tid_detached = is_detached;

    if (lw_thread_track_sync_events) {
        tid->lw_tid_sync_log = lw_sync_log_create();
    } else {
        tid->lw_tid_sync_log = NULL;
    }

    return tid;
}

lw_thread_t lw_thread_self(void)
{
    lw_thread_t tid;
    tid = pthread_getspecific(lw_thread_id_key);
    if (tid == NULL) {
        int ret;
        tid = lw_thread_id_alloc(TRUE);
        ret = pthread_setspecific(lw_thread_id_key, tid);
        lw_verify(ret == 0);
    }
    return tid;
}

static void
lw_thread_id_destructor(void *arg)
{
    lw_thread_t tid = arg;

    if (tid->lw_tid_sync_log != NULL) {
        lw_sync_log_t *lw_sync_log = tid->lw_tid_sync_log;
        tid->lw_tid_sync_log = NULL;
        lw_sync_log_destroy(lw_sync_log);
    }

    if (tid->lw_tid_detached) {
        /* Free this struct */
        free(tid);
    }
}

void
lw_thread_join(lw_thread_t tid,
               void **retval)
{
    int ret;

    lw_assert(!tid->lw_tid_detached);
    ret = pthread_join(tid->lw_tid_pthread_id, retval);
    lw_verify(ret == 0);
    tid->lw_tid_detached = TRUE;

    if (tid->lw_tid_sync_log != NULL) {
        lw_sync_log_t *lw_sync_log = tid->lw_tid_sync_log;
        tid->lw_tid_sync_log = NULL;
        lw_sync_log_destroy(lw_sync_log);
    }

    free(tid);
}

void
lw_thread_cancel(lw_thread_t tid)
{
    LW_IGNORE_RETURN_VALUE(pthread_cancel(tid->lw_tid_pthread_id));
}

void
lw_thread_detach(lw_thread_t tid)
{
    int ret;
    ret = pthread_detach(tid->lw_tid_pthread_id);
    lw_verify(ret == 0);
    tid->lw_tid_detached = TRUE;
}

pthread_t
lw_thread_get_ptid(lw_thread_t tid)
{
    if (tid == NULL) {
        return pthread_self();
    }
    return tid->lw_tid_pthread_id;
}

static void
lw_thread_private_alloc(LW_IN char *name)
{
    lw_thread_t tid;
    tid = lw_thread_self();

    /*
     * Set thread name.
     */
    tid->lw_tid_name = name;

    /*
     * Allocate a thread specific wait structure.
     */
    tid->lw_tid_waiter = lw_waiter_get();
    lw_verify(tid->lw_tid_waiter != NULL);
}

static void
lw_thread_private_free()
{
    /*
     * Free the thread specific wait structure.
     */
    lw_waiter_dealloc_global();
}

/*
 * Internal thread start routine.  This routine acts as a wrapper for
 * the start routine provided by the caller of dd_thread_create().
 * This routine performs thread specific initialization before handing
 * off control.
 */
static void *
_lw_thread_start_func(const char *name,
                      lw_thread_t tid,
                      _lw_thread_arg_t *thargs)
{
    lw_thread_run_func_t start_func = thargs->lw_ta_start_func;
    void *arg = thargs->lw_ta_arg;
    void *retval;
    int ret = 0;

    /*
     * Free the thread arg structure now that we've cached its contents.
     */
    free(thargs);

    lw_thread_private_alloc(name);

    retval = (*start_func)(arg);

    lw_thread_private_free();

    /*
     * This is a hack to keep name from being in a register so it will
     * always be visible in a gdb stacktrace.
     */
    arg = (void *)&name;

    return retval;
}

/*
 * This provides the base of the lw_thread stack so that it can
 * make the thread id and name show up in a gdb stack display
 * of a thread's call stack.
 */
static void *
_lw_thread_base(void *void_tharg)
{
    void *ret;
    _lw_thread_arg_t *thargs = void_tharg;
    lw_thread_t tid = thargs->lw_ta_tid;
    /* Reset pthread_id */
    tid->lw_tid_pthread_id = pthread_self();

    /* Set thread_id_struct for this thread */
    lw_verify(pthread_getspecific(lw_thread_id_key) == NULL);
    lw_verify(pthread_setspecific(lw_thread_id_key, tid) == 0);

    /* set errno ptr */
    tid->lw_tid_errno_ptr = &errno;

    ret = _lw_thread_start_func(thargs->lw_ta_name, tid, thargs);

    /* Take care of cleaning up tid and clear the thread specific
     * value so that the destructor is not called by pthread
     * library. If the thread was prematurely terminated for some
     * reason, the key destructor would be called and that would take
     * care of cleaning up tid 
     */
    lw_verify(pthread_setspecific(lw_thread_id_key, NULL) == 0);
    lw_thread_id_destructor(tid);

    return ret;
}

/**
 * Create a thread
 *
 * @param thread (i) the output parameter specifying a pointer to
 *     a thread id variable.
 * @param start_func (i) the input parameter specifying a pointer to
 *     a function to be called at thread creation time.
 * @param arg (i) the input parameter specifying the pointer to
 *     argument data to be passed to the start function.
 *
 * <br>
 * @description
 * 
 * This interface creates a new thread, which commences execution with a
 * call to the specified start_func.  The arg parameter is passed to
 * start_func.  On success, 0 is returned and otherwise non-zero on error.
 */
int
lw_thread_create(lw_thread_t *thread,
                 pthread_attr_t *attr,
                 lw_thread_run_func_t start_func,
                 void *arg,
                 char const *name)
{
    _lw_thread_arg_t *thargs;
    int ret;
    pthread_attr_t attr2;
    int detach_state = PTHREAD_CREATE_JOINABLE;

    /*
     * Allocate a thread argument structure and copy the caller arguments
     * into the structure.
     */
    thargs = malloc(sizeof(_lw_thread_arg_t));
    lw_verify(thargs != NULL);
    thargs->lw_ta_start_func = start_func;
    thargs->lw_ta_arg = arg;
    thargs->lw_ta_name = name;
    thargs->lw_ta_tid = lw_thread_id_alloc(FALSE);
    *thread = thargs->lw_ta_tid;

    if (attr == NULL) {
        lw_verify(pthread_attr_init(&attr2) == 0);
        attr = &attr2;
    }

    ret = pthread_attr_getdetachstate(attr, &detach_state);
    lw_verify(ret == 0);

    if (detach_state == PTHREAD_CREATE_DETACHED) {
        /* Need to set detached bit on this thread. Do this before the
         * thread is created. The thread could exit immediately and free
         * the pointer.
         */
        (*thread)->lw_tid_detached = TRUE;
    }

    ret = pthread_create(&(thargs->lw_ta_tid->lw_tid_pthread_id),
                         attr,
                         _lw_thread_base,
                         thargs);
    thread = NULL; /* This belongs to created thread now */
    
    if (ret != 0) {
        fprintf(stderr, "%s: pthread_create failed (%d)\n", __func__, ret);
        free(thargs->lw_ta_tid);
    }

    if (attr == &attr2) {
        lw_verify(pthread_attr_destroy(&attr2) == 0);
    }

    return ret;
}


int
dd_thread_create_detached(lw_thread_t *thread,
                          lw_thread_run_func_t start_func,
                          void *arg,
                          char const *name)
{
    pthread_attr_t attr;
    int rc = 0;
    lw_verify(pthread_attr_init(&attr) == 0);
    lw_verify(pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED) == 0);
    rc = lw_thread_create(thread, &attr, start_func, arg, name);
    thread = NULL; /* belongs to created thread now */
    lw_verify(pthread_attr_destroy(&attr) == 0);
    return rc;
}

void
lw_thread_system_init(lw_bool_t track_sync_events)
{
    int ret;
    lw_verify(lw_thread_initialized == FALSE);

    ret = pthread_key_create(&lw_thread_id_key, lw_thread_id_destructor);
    lw_verify(ret == 0);

    lw_thread_track_sync_events = track_sync_events;
    lw_thread_initialized = TRUE;
    
}

void
lw_thread_system_shutdown(void)
{
    int ret;
    lw_verify(lw_thread_initialized == TRUE);

    ret = pthread_key_delete(lw_thread_id_key);
    lw_verify(ret == 0);
    
    lw_thread_initialized = FALSE;
}

lw_sync_log_line_t *
lw_thread_sync_log_next_line(void)
{
    lw_thread_t tid;
    tid = pthread_getspecific(lw_thread_id_key);
    if (tid == NULL || tid->lw_tid_sync_log == NULL) {
        /* Thread was not created with lw_thread api or the sync log
         * feature was not used during lw_thread api initialization
         * (lw_thread_system_init function) */
        return NULL;
    }

    return lw_sync_log_next_line(tid->lw_tid_sync_log);
}
