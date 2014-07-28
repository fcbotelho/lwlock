/***
 * Developed originally at EMC Corporation, this library is released under the
 * MPL 2.0 license.  Please refer to the MPL-2.0 file in the repository for its
 * full description or to http://www.mozilla.org/MPL/2.0/ for the online version.
 *
 * Before contributing to the project one needs to sign the committer agreement
 * available in the "committerAgreement" directory.
 */

#ifndef __LW_EVENT_H__
#define __LW_EVENT_H__

#include "lw_types.h"
#include "lw_dlist.h"
#include "lw_magic.h"
#include <time.h>
#include <sys/time.h>

/**
 * Simple binary event. This is meant to be used between 2 threads only, one of
 * which will wait (the owner) and the other will signal.
 */
typedef struct lw_event_iface_s  lw_event_iface_t;
typedef struct lw_base_event_s   lw_base_event_t;

typedef void * lw_event_t;
#define LW_EVENT_2_IFACE(ev)            ((lw_event_iface_t *)(ev))
#define LW_EVENT_2_BASE_EVENT(ev)       ((lw_base_event_t *)(ev))
#define LW_EVENT_SET(ev, evp)           ((ev) = (evp))

typedef void (*lw_event_signal_func_t)(lw_event_t event, void *arg);
typedef int (*lw_event_wait_func_t)(lw_event_t event,
                                    void *arg,
                                    const struct timespec *abstime);
typedef lw_bool_t (*lw_event_wakeup_pending_func_t)(lw_event_t event, void *arg);

struct lw_event_iface_s {
    lw_delem_t                      link;
#ifdef LW_DEBUG
    lw_magic_t                      magic;
#endif
    lw_event_signal_func_t          signal;
    lw_event_wait_func_t            wait;
    lw_event_wakeup_pending_func_t  wakeup_pending;
};

struct lw_base_event_s {
    lw_event_iface_t  iface;    /* Keep first */
    void              *wait_src;/* Generic pointer set by external libraries.
                                 * Used for debugging info.
                                 */
    lw_uint64_t       tag;      /* To be used by libraries using this struct */
};

/*
 * Signal the event structure so the owner thread can wake up and continue.
 */
static INLINE void
lw_event_signal(LW_INOUT lw_event_t _event,
                LW_INOUT void *arg)
{
    lw_event_iface_t *event = LW_EVENT_2_IFACE(_event);
    lw_assert(event->magic == LW_EVENT_MAGIC);
    event->signal(event, arg);
}

static INLINE int
lw_event_timedwait(LW_INOUT lw_event_t _event,
                   LW_INOUT void *arg,
                   LW_IN struct timespec *abstime)
{
    lw_event_iface_t *event = LW_EVENT_2_IFACE(_event);
    lw_assert(event->magic == LW_EVENT_MAGIC);
    return event->wait(event, arg, abstime);
}

static INLINE void
lw_event_wait(LW_INOUT lw_event_t _event,
              LW_INOUT void *arg)
{
    int ret = lw_event_timedwait(_event, arg, NULL);
    lw_verify(ret == 0);
}

static INLINE lw_bool_t
lw_event_wakeup_pending(LW_INOUT lw_event_t _event,
                        LW_INOUT void *arg)
{
    lw_event_iface_t *event = LW_EVENT_2_IFACE(_event);
    lw_assert(event->magic == LW_EVENT_MAGIC);
    return event->wakeup_pending(event, arg);
}

static INLINE void
lw_event_iface_init(LW_INOUT lw_event_t event,
                    LW_INOUT lw_event_signal_func_t signal,
                    LW_INOUT lw_event_wait_func_t wait,
                    LW_INOUT  lw_event_wakeup_pending_func_t wakeup_pending)
{
    lw_event_iface_t *iface = LW_EVENT_2_IFACE(event);

#ifdef LW_DEBUG
    iface->magic = LW_EVENT_MAGIC;
#endif

    lw_dl_init_elem(&iface->link);
    iface->signal = signal;
    iface->wait = wait;
    iface->wakeup_pending = wakeup_pending;
}

static INLINE void
lw_base_event_init(LW_INOUT lw_base_event_t *base_event,
                   LW_INOUT lw_event_signal_func_t signal,
                   LW_INOUT lw_event_wait_func_t wait,
                   LW_INOUT  lw_event_wakeup_pending_func_t wakeup_pending)
{
    lw_event_iface_init(base_event, signal, wait, wakeup_pending);
    base_event->wait_src = NULL;
    base_event->tag = LW_MAX_UINT64;
}

/**
 * The functions below are meant for use with a lw_base_event_t and use an on stack waiter
 * structure to do the actual waiting. This is used in some libraries where the per thread
 * waiter might be in use elsewhere. The event also uses the wait_src and tag internally, so
 * those are not avaible for any other use.
 */
extern int lw_stack_event_wait(lw_event_t event, void *arg, LW_IN struct timespec *abstime);
extern void lw_stack_event_signal(lw_event_t event, void *arg);
extern lw_bool_t lw_stack_event_wakeup_pending(lw_event_t event, void *arg);

static INLINE void
lw_stack_base_event_init(LW_INOUT lw_base_event_t *base_event)
{
    lw_base_event_init(base_event, lw_stack_event_signal, lw_stack_event_wait,
                       lw_stack_event_wakeup_pending);
    base_event->tag = 2;
}

#endif

