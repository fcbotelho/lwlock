#ifndef __LW_EVENT_H__
#define __LW_EVENT_H__

#include "lw_types.h"
#include "lw_dlist.h"
#include "lw_magic.h"
#include <time.h>
/**
 * Simple binary event. This is meant to be used between 2 threads only, one of
 * which will wait (the owner) and the other will signal.
 */
typedef struct lw_event_iface_s  lw_event_iface_t;
typedef struct lw_base_event_s   lw_base_event_t;
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

#define LW_EVENT_MAGIC      LW_MAGIC(0x959)

struct lw_event_iface_s {
    lw_delem_t                      lw_event_iface_link;
#ifdef LW_DEBUG
    lw_magic_t                      lw_event_iface_magic;
#endif    
    lw_event_signal_func_t          lw_event_iface_signal;
    lw_event_wait_func_t            lw_event_iface_wait;
    lw_event_wakeup_pending_func_t  lw_event_iface_wakeup_pending;
};

struct lw_base_event_s {
    lw_event_iface_t  lw_base_event_iface; /* Keep first */
    /*
     * Generic pointer set by external libraries.
     * Used for debugging info.
     */
    void              *lw_base_event_wait_src;   
    /* To be used by libraries using this struct */
    lw_uint64_t       lw_base_event_tag; 
};

struct lw_thread_event_s {
    lw_base_event_t  lw_thread_event_base;
    lw_bool_t        lw_thread_event_signal_pending;
    lw_bool_t        lw_thread_event_waiter_waiting;
    lw_boo_t         lw_thread_event_trace_history;
    pthread_mutex_t  lw_thread_event_mutex;
    pthread_cond_t   lw_thread_event_cond;
#ifdef LW_DEBUG
    void             *lw_thread_event_tid;
#endif
};

/*
 * Signal the private event structure of a thread so it will wake up and
 * continue.
 */
static inline void
lw_event_signal(lw_event_t _event, void *arg)
{
    lw_event_iface_t *event = LW_EVENT_2_IFACE(_event);
    lw_assert(event->lw_event_iface_magic == LW_EVENT_MAGIC);
    event->lw_event_iface_signal(event, arg);
}
 
static inline int
lw_event_timedwait(lw_event_t _event,
                   void *arg,
                   const struct timespec *abstime)
{
    lw_event_iface_t *event = DD_EVENT_2_IFACE(_event);
    lw_asserta(event->lw_event_iface_magic == LW_EVENT_MAGIC);
    return event->lw_event_iface_wait(event, arg, abstime);
}

static inline void
lw_event_wait(lw_event_t _event, void *arg)
{
    int ret = lw_event_timedwait(_event, arg, NULL);
    dd_verify(ret == 0);
}

static inline dd_bool_t
dd_event_wakeup_pending(dd_event_t _event, void *arg)
{
    dd_event_iface_t *event = DD_EVENT_2_IFACE(_event);
    lw_assert(event->lw_event_iface_magic == LW_EVENT_MAGIC);
    return event->lw_event_iface_wakeup_pending(event, arg);
}
static inline void
lw_base_event_init(lw_base_event_t *base_event,
                   lw_event_signal_func_t signal,
                   lw_event_wait_func_t wait,
                   lw_event_wakeup_pending_func_t wakeup_pending)
{
    lw_dl_init_elem(&base_event->lw_base_event_iface.lw_event_iface_link);

#ifdef LW_DEBUG
    base_event->lw_base_event_iface.lw_event_iface_magic = LW_EVENT_MAGIC;
#endif

    base_event->lw_base_event_iface.lw_event_iface_signal = signal;
    base_event->lw_base_event_iface.lw_event_iface_wait = wait;
    base_event->lw_base_event_iface.lw_event_iface_wakeup_pending = wakeup_pending;
    base_event->lw_base_event_wait_src = NULL;
    base_event->lw_base_event_tag = LW_MAX_UINT64;
}

extern void lw_thread_event_init(dd_thread_event_t *thread_event);
extern void lw_thread_event_destroy(dd_thread_event_t *thread_event);

#endif

