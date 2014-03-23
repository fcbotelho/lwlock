/***
 * Developed originally at EMC Corporation, this library is released under the
 * MPL 2.0 license.  Please refer to the MPL-2.0 file in the repository for its
 * full description or to http://www.mozilla.org/MPL/2.0/ for the online version.
 *
 * Before contributing to the project one needs to sign the committer agreement
 * available in the "committerAgreement" directory.
 */

#ifndef __LW_WAITER_H__
#define __LW_WAITER_H__

#include "lw_types.h"
#include "lw_event.h"

/**
 * Thread wait domains: Each domain can have upto LW_WAITER_ID_MAX waiters.
 * The domain provides functions to allocate/free/get waiter structs for that
 * domain.
 */
typedef struct lw_waiter_domain_s lw_waiter_domain_t;

typedef struct {
    lw_thread_event_t  event;     /* Event to synchronize on */
    lw_waiter_domain_t *domain;
    lw_waiter_id_t     id;        /* Id of this struct */
    lw_waiter_id_t     next;      /* Id of next struct when in list */
    lw_waiter_id_t     prev;      /* Id of prev struct when in list */
    lw_bool_t          initialized; /* Structure is valid and initialized */
} lw_waiter_t;

typedef lw_waiter_t *
(*lw_waiter_alloc_func_t)(LW_INOUT lw_waiter_domain_t *domain);

typedef void
(*lw_waiter_free_func_t)(LW_INOUT lw_waiter_domain_t *domain,
                         LW_INOUT lw_waiter_t *waiter);
typedef lw_waiter_t *
(*lw_waiter_get_func_t)(LW_INOUT lw_waiter_domain_t *domain);

typedef lw_waiter_t *
(*lw_waiter_from_id_func_t)(LW_INOUT lw_waiter_domain_t *domain,
                            LW_IN lw_uint32_t id);

struct lw_waiter_domain_s {
    lw_waiter_alloc_func_t    lw_wd_alloc_waiter;
    lw_waiter_free_func_t     lw_wd_free_waiter;
    lw_waiter_get_func_t      lw_wd_get_waiter;
    lw_waiter_from_id_func_t  lw_wd_id2waiter;
    void                      *lw_wd_opaque;
};

extern lw_waiter_domain_t  *lw_waiter_global_domain;

static inline lw_waiter_t *
lw_waiter_alloc(void)
{
    return lw_waiter_global_domain->lw_wd_alloc_waiter(lw_waiter_global_domain);
}

static inline void
lw_waiter_free(LW_INOUT void *arg)
{
    lw_waiter_t *waiter = arg;
    waiter->domain->lw_wd_free_waiter(waiter->domain, waiter);
}

static inline lw_waiter_t *
lw_waiter_get(void)
{
    return lw_waiter_global_domain->lw_wd_get_waiter(lw_waiter_global_domain);
}

static inline lw_waiter_t *
lw_waiter_from_id(LW_IN lw_uint32_t id)
{
    return lw_waiter_global_domain->lw_wd_id2waiter(lw_waiter_global_domain,
                                                    id);
}

static inline void
lw_waiter_wait(LW_INOUT lw_waiter_t *waiter)
{
    lw_event_wait(&waiter->event,
                  waiter->event.base.wait_src);
}

static inline int
lw_waiter_timedwait(LW_INOUT lw_waiter_t *waiter,
                    LW_IN struct timespec *abstime)
{
    return lw_event_timedwait(&waiter->event,
                              waiter->event.base.wait_src,
                              abstime);
}

static inline void
lw_waiter_wakeup(LW_INOUT lw_waiter_t *waiter,
                 LW_INOUT void *arg)
{
    lw_event_signal(&waiter->event, arg);
}

static inline void
lw_waiter_wake_all(LW_INOUT lw_waiter_domain_t *domain,
                   LW_IN lw_uint32_t _id,
                   LW_INOUT void *arg)
{
    lw_waiter_t *waiter = NULL;
    lw_waiter_id_t id;
    lw_assert(_id <= LW_WAITER_ID_MAX);
    id = (lw_waiter_id_t) _id;
    if (domain == NULL) {
        domain = lw_waiter_global_domain;
        lw_verify(domain != NULL);
    }
    while (id < LW_WAITER_ID_MAX) {
        waiter = domain->lw_wd_id2waiter(domain, id);
        lw_assert(waiter->initialized);
        id = waiter->next;
        waiter->next = LW_WAITER_ID_MAX;
        waiter->prev = LW_WAITER_ID_MAX;
        lw_event_signal(&waiter->event, arg);
    }
}

extern void
lw_waiter_dealloc_global(void);

/* Initialize the global waiter domain */
extern void
lw_waiter_domain_init_global(lw_waiter_domain_t *domain);

/* Shutdown the global waiter domain */
extern void
lw_waiter_domain_shutdown_global(void);

#endif
