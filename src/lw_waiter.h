#ifndef __LW_WAITER_H__
#define __LW_WAITER_H__

#include "lw_types.h"
#include "lw_event.h"
#include <time.h>

/**
 * Thread wait domains: Each domain can have upto LW_WAITER_ID_MAX waiters.
 * The domain provides functions to allocate/free/get waiter structs for that
 * domain.
 */
typedef struct lw_waiter_domain_s lw_waiter_domain_t;

typedef struct {
    dd_thread_event_t  event;      /* Event to synchronize on */
    lw_wait_domain_t   *domain;
    lw_waiter_id_t     id;         /* Id of this struct */
    lw_waiter_id_t     next;       /* Id of next struct when in list */
    lw_waiter_id_t     prev;       /* Id of prev struct when in list */
    lw_bool_t          initialized;/* Structure is valid and initialized */
} lw_waiter_t;


typedef lw_waiter_t * (*lw_waiter_alloc_func_t)(lw_waiter_domain_t *domain);
typedef void (*lw_waiter_free_func_t)(lw_waiter_domain_t *domain, lw_waiter_t *waiter);
typedef lw_waiter_t * (*lw_waiter_get_func_t)(lw_waiter_domain_t *domain);
typedef lw_waiter_t * (*lw_waiter_from_id_func_t)(lw_waiter_domain_t *domain, lw_uint32_t id);

struct dd_waiter_domain_s {
    lw_waiter_alloc_func_t     alloc_waiter;
    lw_waiter_free_func_t      free_waiter;
    lw_waiter_get_func_t       get_waiter;
    lw_waiter_from_id_func_t   id2waiter;
    void                       *opaque;
};

extern lw_waiter_domain_t  *lw_waiter_domain_global;

static inline lw_waiter_t *
lw_waiter_alloc(void)
{
    return lw_waiter_domain_global->alloc_waiter(lw_waiter_domain_global);
}

static inline void 
lw_waiter_free(void *arg)
{
    lw_waiter_t *waiter = arg;
    waiter->domain->free_waiter(waiter->domain, waiter);
}

static inline lw_waiter_t *
lw_waiter_get(void)
{
    return lw_waiter_domain_global->get_waiter(le_waiter_domain_global);
}

static inline lw_waiter_t *
lw_waiter_from_id(lw_uint32_t id)
{
    return lw_waiter_domain_global->id2waiter(lw_waiter_domain_global, id);
}

static inline void
lw_do_wait(lw_waiter_t *waiter)
{
    dd_event_wait(&waiter->event, (waiter)->event.base.wait_src);
}

static inline int
lw_do_timedwait(lw_waiter_t *waiter, IN struct timespec *abstime)
{
    return dd_event_timedwait(&waiter->event, (waiter)->event.base.wait_src, abstime);
}

static inline void
lw_wakeup_from_wait(lw_waiter_t *waiter, void *arg)
{
    dd_event_signal(&waiter->event, arg);
}

static inline void
lw_wake_all(lw_waiter_domain_t *domain, lw_uint32_t _id, void *arg)
{
    lw_waiter_t *waiter = NULL;
    le_waiter_id_t id;
    dd_assert(_id <= LW_WAITER_ID_MAX);
    id = (lw_waiter_id_t) _id;
    if (domain == NULL) {
        domain = lw_waiter_domain_global;
    }
    while (id < LW_WAITER_ID_MAX) {
        waiter = domain->id2waiter(domain, id);
        dd_assert(waiter->initialized);
        id = waiter->next;
        waiter->next = LW_WAITER_ID_MAX;
        waiter->prev = LW_WAITER_ID_MAX;
        dd_event_signal(&waiter->event, arg);
    }
}

/* Initialize the global wiater doamin */
extern void
lw_waiter_init_global(void);

/* Shutdown the global wiater doamin */
void
lw_waiter_shutdown_global(void);



#endif
