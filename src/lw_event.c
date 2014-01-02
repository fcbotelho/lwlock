#include "lw_event.h"
#include "lw_debug.h"
#include <pthread.h>
#include <errno.h>

static void
lw_thread_event_signal(LW_INOUT lw_event_t _event, 
                       LW_INOUT void *arg);

static int
lw_thread_event_wait(LW_INOUT lw_event_t _event,
                     LW_INOUT void *arg,
                     LW_INOUT const struct timespec *abstime);

static lw_bool_t
lw_thread_event_wakeup_pending(LW_INOUT lw_event_t _event, 
                               LW_INOUT void *arg);

static void
lw_thread_event_signal(LW_INOUT lw_event_t _event, 
                       LW_INOUT void *arg)
{
    LW_UNUSED_PARAMETER(arg);
    lw_thread_event_t *event = LW_EVENT_2_THREAD_EVENT(_event);
    lw_assert(lw_thread_event_wait ==
              event->lw_te_base.lw_be_iface.lw_ei_wait); 
    lw_verify(pthread_mutex_lock(&event->lw_te_mutex) == 0);
    lw_verify(!event->lw_te_signal_pending);
    event->lw_te_signal_pending = TRUE;
    if (event->lw_te_waiter_waiting) {
        lw_verify(pthread_cond_signal(&event->lw_te_cond) == 0);
    }
    lw_verify(pthread_mutex_unlock(&event->lw_te_mutex) == 0);
}

static lw_bool_t
lw_thread_event_wakeup_pending(LW_INOUT lw_event_t _event, 
                               LW_INOUT void *arg)
{
    LW_UNUSED_PARAMETER(arg);
    lw_thread_event_t *event = LW_EVENT_2_THREAD_EVENT(_event);
    lw_assert(lw_thread_event_signal ==
              event->lw_te_base.lw_be_iface.lw_ei_signal);
    lw_assert(lw_thread_event_wait == 
              event->lw_te_base.lw_be_iface.lw_ei_wait);
    return event->lw_te_signal_pending;
}

/**
 * Called by a thread on its private event structure to wait for some other
 * thread to wake it up.
 */
static int
lw_thread_event_wait(LW_INOUT lw_event_t _event,
                     LW_INOUT void *arg,
                     LW_IN struct timespec *abstime)
{
    int ret = 0;
    lw_thread_event_t *event = LW_EVENT_2_THREAD_EVENT(_event);
#ifdef LW_DEBUG
    lw_assert(event->lw_te_tid == lw_thread_self());
#endif
    lw_assert(lw_thread_event_signal ==
              event->lw_te_base.lw_be_iface.lw_ei_signal);
    void *src = event->lw_te_base.lw_be_wait_src;
    lw_verify(pthread_mutex_lock(&event->lw_te_mutex) == 0);
    lw_verify(!event->lw_te_waiter_waiting);
    event->lw_te_waiter_waiting = TRUE;
    if (abstime == NULL) {
        while (!event->lw_te_signal_pending) {
            lw_verify(pthread_cond_wait(&event->lw_te_cond, 
                                        &event->lw_te_mutex) == 0);
        }
    } else {
        while (!event->lw_te_signal_pending) {
            ret = pthread_cond_timedwait(&event->lw_te_cond, 
                                         &event->lw_te_mutex, 
                                         abstime);
            lw_verify(ret == 0 || ret == EINTR || ret == ETIMEDOUT);
            if (event->lw_te_signal_pending) {
                /* Woken up by actual signal or timeout. But regardless,
                 * we have received a signal.
                 */
                ret = 0;
                break;
            } else if (ret == ETIMEDOUT) {
                /* Need to bail out */
                break;
            }
        } /* While !signal */
    } /* if abstime (timedwait) */

    event->lw_te_waiter_waiting = FALSE;
    event->lw_te_signal_pending = FALSE;
    lw_verify(pthread_mutex_unlock(&event->lw_te_mutex) == 0);
    lw_verify(event->lw_te_base.lw_be_wait_src == src ||
              event->lw_te_base.lw_be_wait_src == arg);
    event->lw_te_base.lw_be_wait_src = NULL;

    if (event->lw_te_trace_history) {
        // TODO: trace history for debugging
    }
    return ret;
}

void
lw_thread_event_init(LW_INOUT lw_thread_event_t *thread_event)
{   
    int ret;
    pthread_condattr_t cond_attr;

    ret = pthread_mutex_init(&thread_event->lw_te_mutex, NULL);
    lw_verify(ret == 0);
    ret = pthread_condattr_init(&cond_attr);
    lw_verify(ret == 0);

//     #define CLOCK_MONOTONIC 1
//     ret = pthread_condattr_setclock(&cond_attr, CLOCK_MONOTONIC);
//     lw_verify(ret == 0);
// 
    ret = pthread_cond_init(&thread_event->lw_te_cond, &cond_attr);
    lw_verify(ret == 0);
    ret = pthread_condattr_destroy(&cond_attr);
    lw_verify(ret == 0);

    thread_event->lw_te_trace_history  = TRUE; 
    thread_event->lw_te_signal_pending = FALSE;
    thread_event->lw_te_waiter_waiting = FALSE;
#ifdef LW_DEBUG
    thread_event->lw_te_tid = lw_thread_self();
#endif
    lw_base_event_init(&thread_event->lw_te_base,
                       lw_thread_event_signal,
                       lw_thread_event_wait,
                       lw_thread_event_wakeup_pending);
}

void
lw_thread_event_destroy(LW_INOUT lw_thread_event_t *event)
{
    lw_verify(event->lw_te_base.lw_be_wait_src == NULL); 
    lw_verify(!event->lw_te_signal_pending);
    lw_verify(!event->lw_te_waiter_waiting);
    lw_verify(pthread_mutex_destroy(&event->lw_te_mutex) == 0);
    lw_verify(pthread_cond_destroy(&event->lw_te_cond) == 0);
#ifdef LW_DEBUG
    event->lw_te_tid = NULL;
    event->lw_te_base.lw_be_iface.lw_ei_magic = 0;
#endif
}
