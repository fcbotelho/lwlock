#include "lw_event.h"
#include <pthread.h>
#include <errno.h>

static void
lw_thread_event_signal(lw_event_t _event, void *arg)
{
    lw_thread_event_t *event = LW_EVENT_2_THREAD_EVENT(_event);
    lw_assert(event->base.iface.wait_cb == lw_thread_event_wait);
    LW_UNUSED_PARAMETER(arg);
    lw_verify(pthread_mutex_lock(&event->lte_mutex) == 0);
    lw_verify(!event->signal_pending);
    event->signal_pending = TRUE;
    if (event->waiter_waiting) {
        lw_verify(pthread_cond_signal(&event->lte_cond) == 0);
    }
    lw_verify(pthread_mutex_unlock(&event->lte_mutex) == 0);
}

static lw_bool_t
lw_thread_event_wakeup_pending(lw_event_t _event, void *arg)
{
    lw_thread_event_t *event = LW_EVENT_2_THREAD_EVENT(_event);
    LW_UNUSED_PARAMETER(arg);
    lw_assert(event->base.iface.signal_cb == lw_thread_event_signal);
    lw_assert(event->base.iface.wait_cb == lw_thread_event_wait);
    return event->signal_pending;
}

/**
 * Called by a thread on its private event structure to wait for some other
 * thread to wake it up.
 */
static int
lw_thread_event_wait(lw_event_t _event,
                     void *arg,
                     const struct timespec *abstime)
{
    int ret = 0;
    lw_thread_event_t *event = DD_EVENT_2_THREAD_EVENT(_event);
    lw_assert(event->base.iface.signal_cb == lw_thread_event_signal);
    void *src = event->base.wait_src;
    lw_verify(pthread_mutex_lock(&event->lte_mutex) == 0);
    lw_verify(!event->waiter_waiting);
    event->waiter_waiting = TRUE;
    if (abstime == NULL) {
        while (!event->signal_pending) {
            lw_verify(pthread_cond_wait(&event->lte_cond, &event->lte_mutex) == 0);
        }
    } else {
        while (!event->signal_pending) {
            ret = pthread_cond_timedwait(&event->lte_cond, &event->lte_mutex, abstime);
            dd_verify(ret == 0 || ret == EINTR || ret == ETIMEDOUT);
            if (event->signal_pending) {
                /* Woken up by actual signal or timeout. But regardless,
                 * we have a receieved a signal.
                 */
                ret = 0;
                break;
            } else if (ret == ETIMEDOUT) {
                /* Need to bail out */
                break;
            }
        } /* While !signal */
    } /* if abstime (timedwait) */

    event->waiter_waiting = FALSE;
    event->signal_pending = FALSE;
    lw_verify(pthread_mutex_unlock(&event->lte_mutex) == 0);
    lw_verify(event->base.wait_src == src ||
              event->base.wait_src == arg);
    event->base.wait_src = NULL;

    return ret;
}

void
lw_thread_event_init(lw_thread_event_t *thread_event)
{   
    int ret;
    pthread_condattr_t cond_attr;

    ret = pthread_mutex_init(&thread_event->lte_mutex, NULL);
    lw_verify(ret == 0);
    ret = pthread_condattr_init(&cond_attr);
    lw_verify(ret == 0);
    ret = pthread_cond_init(&thread_event->lte_cond, &cond_attr);
    lw_verify(ret == 0);
    ret = pthread_condattr_destroy(&cond_attr);
    lw_verify(ret == 0);
 
    thread_event->signal_pending = FALSE;
    thread_event->waiter_waiting = FALSE;

    lw_base_event_init(&thread_event->base,
                       lw_thread_event_signal,
                       lw_thread_event_wait,
                       lw_thread_event_wakeup_pending);
}

void
lw_thread_event_destroy(lw_thread_event_t *event)
{
    lw_verify(event->base.wait_src == NULL); 
    lw_verify(!event->signal_pending);
    lw_verify(!event->waiter_waiting);
    lw_verify(pthread_mutex_destroy(&event->lte_mutex) == 0);
    lw_verify(pthread_cond_destroy(&event->lte_cond) == 0);
}
