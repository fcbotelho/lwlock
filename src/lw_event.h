#ifndef __LW_EVENT_H__
#define __LW_EVENT_H__

#include "lw_types.h"
#include "lw_dlist.h"
#include <time.h>

/**
 * Simple binary event. This is meant to be used between 2 threads only, one of
 * which will wait (the owner) and the other will signal.
 */
typedef struct lw_event_iface_s lw_event_iface_t;
typedef struct lw_base_event_s lw_base_event_t;
typedef struct lw_thread_event_s lw_thread_event_t;


typedef void * lw_event_t;
#define LW_EVENT_2_IFACE(ev)            ((lw_event_iface_t *)(ev))
#define LW_EVENT_2_BASE_EVENT(ev)       ((lw_base_event_t *)(ev))
#define LW_EVENT_2_THREAD_EVENT(ev)     ((lw_thread_event_t *)(ev))
#define LW_EVENT_SET(ev, evp)           ((ev) = (evp))

typedef void (*lw_event_signal_func_t)(lw_event_t event, void *arg);
typedef int (*lw_event_wait_func_t)(lw_event_t event,
                                    void *arg,
                                    const struct timespec *abstime);
typedef lw_bool_t (*lw_event_wakeup_pending_func_t)(lw_event_t event, void *arg);

struct lw_event_iface_s {
    delem_t                         link;
    lw_event_signal_func_t          signal_cb;
    lw_event_wait_func_t            wait_cb;
    lw_event_wakeup_pending_func_t  wakeup_pending;
};

struct lw_base_event_s {
    lw_event_iface_t                iface; /* Keep first */
    void                            *wait_src;  /* Generic pointer set by external libraries.
                                                 * Used for debugging info.
                                                 */
    lw_uint64_t                     tag;        /* To be used by libraries using this struct */
};

struct lw_thread_event_s {
    lw_base_event_t                 base;
    lw_bool_t                       signal_pending;
    lw_bool_t                       waiter_waiting;
    pthread_mutex_t                 lte_mutex;
    pthread_cond_t                  lte_cond;
};

/*
 * Signal the private event structure of a thread so it will wake up and
 * continue.
 */
static inline void
lw_event_signal(lw_event_t _event, void *arg)
{
    lw_event_iface_t *event = LW_EVENT_2_IFACE(_event);
    event->signal_cb(event, arg);
}
 
static inline int
lw_event_timedwait(lw_event_t _event,
                   void *arg,
                   const struct timespec *abstime)
{
    lw_event_iface_t *event = DD_EVENT_2_IFACE(_event);
    return event->wait_cb(event, arg, abstime);
}

static inline void
lw_event_wait(lw_event_t _event, void *arg)
{
    int ret = lw_event_timedwait(_event, arg, NULL);
    dd_verify(ret == 0);
}

static inline void
lw_base_event_init(lw_base_event_t *base_event,
                   lw_event_signal_func_t signal_cb,
                   lw_event_wait_func_t wait_cb,
                   lw_event_wakeup_pending_func_t wakeup_pending)
{
    dl_init_elem(&base_event->iface.link);

    base_event->iface.signal_cb = signal_cb;
    base_event->iface.wait_cb = wait_cb;
    base_event->iface.wakeup_pending = wakeup_pending;
    base_event->wait_src = NULL;
    base_event->tag = (lw_uint64_t)(-1ULL);
}

extern void lw_thread_event_init(dd_thread_event_t *thread_event);
extern void lw_thread_event_destroy(dd_thread_event_t *thread_event);

#endif

