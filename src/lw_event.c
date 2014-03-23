/***
 * Developed originally at EMC Corporation, this library is released under the
 * MPL 2.0 license.  Please refer to the MPL-2.0 file in the repository for its
 * full description or to http://www.mozilla.org/MPL/2.0/ for the online version.
 *
 * Before contributing to the project one needs to sign the committer agreement
 * available in the "committerAgreement" directory.
 */

#include "lw_event.h"
#include "lw_debug.h"
#include "lw_cycles.h"
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
              event->base.iface.wait);
    lw_verify(pthread_mutex_lock(&event->mutex) == 0);
    lw_verify(!event->signal_pending);
    event->signal_pending = TRUE;
    if (event->waiter_waiting) {
        lw_verify(pthread_cond_signal(&event->cond) == 0);
    }
    lw_verify(pthread_mutex_unlock(&event->mutex) == 0);
}

static lw_bool_t
lw_thread_event_wakeup_pending(LW_INOUT lw_event_t _event,
                               LW_INOUT void *arg)
{
    LW_UNUSED_PARAMETER(arg);
    lw_thread_event_t *event = LW_EVENT_2_THREAD_EVENT(_event);
    lw_assert(lw_thread_event_signal ==
              event->base.iface.signal);
    lw_assert(lw_thread_event_wait ==
              event->base.iface.wait);
    return event->signal_pending;
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
    lw_assert(event->tid == lw_thread_self());
#endif
    lw_assert(lw_thread_event_signal ==
              event->base.iface.signal);
    void *src = event->base.wait_src;
    lw_verify(pthread_mutex_lock(&event->mutex) == 0);
    lw_verify(!event->waiter_waiting);
    event->waiter_waiting = TRUE;
    if (abstime == NULL) {
        while (!event->signal_pending) {
            lw_verify(pthread_cond_wait(&event->cond,
                                        &event->mutex) == 0);
        }
    } else {
        while (!event->signal_pending) {
            ret = pthread_cond_timedwait(&event->cond,
                                         &event->mutex,
                                         abstime);
            lw_verify(ret == 0 || ret == EINTR || ret == ETIMEDOUT);
            if (event->signal_pending) {
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

    event->waiter_waiting = FALSE;
    event->signal_pending = FALSE;
    lw_verify(pthread_mutex_unlock(&event->mutex) == 0);
    lw_verify(event->base.wait_src == src ||
              event->base.wait_src == arg);
    event->base.wait_src = NULL;
    return ret;
}

void
lw_thread_event_init(LW_INOUT lw_thread_event_t *thread_event)
{
    int ret;
    pthread_condattr_t cond_attr;

    ret = pthread_mutex_init(&thread_event->mutex, NULL);
    lw_verify(ret == 0);
    ret = pthread_condattr_init(&cond_attr);
    lw_verify(ret == 0);

//     #define CLOCK_MONOTONIC 1
//     ret = pthread_condattr_setclock(&cond_attr, CLOCK_MONOTONIC);
//     lw_verify(ret == 0);
//
    ret = pthread_cond_init(&thread_event->cond, &cond_attr);
    lw_verify(ret == 0);
    ret = pthread_condattr_destroy(&cond_attr);
    lw_verify(ret == 0);

    thread_event->signal_pending = FALSE;
    thread_event->waiter_waiting = FALSE;
#ifdef LW_DEBUG
    thread_event->tid = lw_thread_self();
#endif
    lw_base_event_init(&thread_event->base,
                       lw_thread_event_signal,
                       lw_thread_event_wait,
                       lw_thread_event_wakeup_pending);
}

void
lw_thread_event_destroy(LW_INOUT lw_thread_event_t *event)
{
    lw_verify(event->base.wait_src == NULL);
    lw_verify(!event->signal_pending);
    lw_verify(!event->waiter_waiting);
    lw_verify(pthread_mutex_destroy(&event->mutex) == 0);
    lw_verify(pthread_cond_destroy(&event->cond) == 0);
#ifdef LW_DEBUG
    event->tid = NULL;
    event->base.iface.magic = 0;
#endif
}
