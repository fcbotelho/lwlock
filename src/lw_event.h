#ifndef __LW_EVENT_H__
#define __LW_EVENT_H__

/*
 *  Copyright(c) 2013 Data Domain, Inc.  All rights reserved.
 *   
 *  DATA DOMAIN CONFIDENTIAL -- This is an unpublished work of Data Domain, Inc.,
 *  and is fully protected under copyright and trade secret laws.  You may not view,
 *  use, disclose, copy, or distribute this file or any information herein except
 *  pursuant to a valid written license from Data Domain.
 */     

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
    lw_delem_t                      lw_ei_link;
#ifdef LW_DEBUG
    lw_magic_t                      lw_ei_magic;
#endif    
    lw_event_signal_func_t          lw_ei_signal;
    lw_event_wait_func_t            lw_ei_wait;
    lw_event_wakeup_pending_func_t  lw_ei_wakeup_pending;
};

struct lw_base_event_s {
    lw_event_iface_t  lw_be_iface; /* Keep first */
    /*
     * Generic pointer set by external libraries.
     * Used for debugging info.
     */
    void              *lw_be_wait_src;   
    /* To be used by libraries using this struct */
    lw_uint64_t       lw_be_tag; 
};

struct lw_thread_event_s {
    lw_base_event_t  lw_te_base;
    lw_bool_t        lw_te_signal_pending;
    lw_bool_t        lw_te_waiter_waiting;
    lw_boo_t         lw_te_trace_history;
    pthread_mutex_t  lw_te_mutex;
    pthread_cond_t   lw_te_cond;
#ifdef LW_DEBUG
    void             *lw_te_tid;
#endif
};

/*
 * Signal the private event structure of a thread so it will wake up and
 * continue.
 */
static inline void
lw_event_signal(LW_INOUT lw_event_t _event, 
                LW_INOUT void *arg)
{
    lw_event_iface_t *event = LW_EVENT_2_IFACE(_event);
    lw_assert(event->lw_ei_magic == LW_EVENT_MAGIC);
    event->lw_ei_signal(event, arg);
}
 
static inline int
lw_event_timedwait(LW_INOUT lw_event_t _event,
                   LW_INOUT void *arg,
                   LW_IN struct timespec *abstime)
{
    lw_event_iface_t *event = DD_EVENT_2_IFACE(_event);
    lw_asserta(event->lw_ei_magic == LW_EVENT_MAGIC);
    return event->lw_ei_wait(event, arg, abstime);
}

static inline void
lw_event_wait(LW_INOUT lw_event_t _event, 
              LW_INOUT void *arg)
{
    int ret = lw_event_timedwait(_event, arg, NULL);
    dd_verify(ret == 0);
}

static inline dd_bool_t
dd_event_wakeup_pending(LW_INOUT dd_event_t _event, 
                        LW_INOUT void *arg)
{
    dd_event_iface_t *event = DD_EVENT_2_IFACE(_event);
    lw_assert(event->lw_ei_magic == LW_EVENT_MAGIC);
    return event->lw_ei_wakeup_pending(event, arg);
}
static inline void
lw_base_event_init(LW_INOUT lw_base_event_t *base_event,
                   LW_INOUT lw_event_signal_func_t signal,
                   LW_INOUT lw_event_wait_func_t wait,
                   LW_IOUT  lw_event_wakeup_pending_func_t wakeup_pending)
{
    lw_dl_init_elem(&base_event->lw_be_iface.lw_ei_link);

#ifdef LW_DEBUG
    base_event->lw_be_iface.lw_ei_magic = LW_EVENT_MAGIC;
#endif

    base_event->lw_be_iface.lw_ei_signal = signal;
    base_event->lw_be_iface.lw_ei_wait = wait;
    base_event->lw_be_iface.lw_ei_wakeup_pending = wakeup_pending;
    base_event->lw_be_wait_src = NULL;
    base_event->lw_be_tag = LW_MAX_UINT64;
}

extern void lw_thread_event_init(LW_INOUT dd_thread_event_t *thread_event);
extern void lw_thread_event_destroy(LW_INOUT dd_thread_event_t *thread_event);

#endif

