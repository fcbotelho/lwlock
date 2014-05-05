/***
 * Developed originally at EMC Corporation, this library is released under the
 * MPL 2.0 license.  Please refer to the MPL-2.0 file in the repository for its
 * full description or to http://www.mozilla.org/MPL/2.0/ for the online version.
 *
 * Before contributing to the project one needs to sign the committer agreement
 * available in the "committerAgreement" directory.
 */

#include "lw_waiter.h"
#include "lw_dlist.h"
#include "lw_debug.h"
#include <pthread.h>
#include <errno.h>

/* Default event used by default waiter domain. Uses pthread mutex + cond var to block / unblock. */
typedef struct lw_thread_event_s {
    lw_base_event_t  base;
    lw_bool_t        signal_pending;
    lw_bool_t        waiter_waiting;
    pthread_mutex_t  mutex;
    pthread_cond_t   cond;
} lw_thread_event_t;

#define LW_EVENT_2_THREAD_EVENT(ev)     ((lw_thread_event_t *)(ev))

typedef struct {
    lw_waiter_t _waiter;
    lw_uint8_t  pad_for_thread_event[sizeof(lw_thread_event_t) - sizeof(lw_base_event_t)];
} lw_waiter_global_t;

/* Functions used by thread event. */
static void lw_thread_event_signal(LW_INOUT lw_event_t _event, LW_INOUT void *arg);
static int
lw_thread_event_wait(LW_INOUT lw_event_t _event,
                     LW_INOUT void *arg,
                     LW_INOUT const struct timespec *abstime);
static lw_bool_t lw_thread_event_wakeup_pending(LW_INOUT lw_event_t _event, LW_INOUT void *arg);

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

static void
lw_waiter_global_event_init(LW_INOUT lw_waiter_domain_t *domain,
                            LW_INOUT lw_waiter_t *waiter)
{
    int ret;
    pthread_condattr_t cond_attr;
    lw_thread_event_t *thread_event = (lw_thread_event_t *)&waiter->event;

    lw_assert(domain == &(lw_global_waiters_domain.lw_wgd_domain));
    LW_UNUSED_PARAMETER(domain);

    ret = pthread_mutex_init(&thread_event->mutex, NULL);
    lw_verify(ret == 0);
    ret = pthread_condattr_init(&cond_attr);
    lw_verify(ret == 0);

    ret = pthread_condattr_setclock(&cond_attr, CLOCK_MONOTONIC);
    lw_verify(ret == 0);

    ret = pthread_cond_init(&thread_event->cond, &cond_attr);
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

static void
lw_waiter_global_event_destroy(LW_INOUT lw_waiter_domain_t *domain,
                               LW_INOUT lw_waiter_t *waiter)
{
    lw_thread_event_t *event = (lw_thread_event_t *)&waiter->event;

    lw_assert(domain == &(lw_global_waiters_domain.lw_wgd_domain));
    LW_UNUSED_PARAMETER(domain);

    lw_verify(event->base.wait_src == NULL);
    lw_verify(!event->signal_pending);
    lw_verify(!event->waiter_waiting);
    lw_verify(pthread_mutex_destroy(&event->mutex) == 0);
    lw_verify(pthread_cond_destroy(&event->cond) == 0);
#ifdef LW_DEBUG
    event->base.iface.magic = 0;
#endif
}

static lw_waiter_t *
lw_waiter_domain_alloc_global(LW_INOUT lw_waiter_domain_t *domain);

static lw_waiter_t *
lw_waiter_domain_get_global(LW_INOUT lw_waiter_domain_t *domain);

static lw_waiter_t *
lw_waiter_domain_from_id_global(LW_INOUT lw_waiter_domain_t *domain,
                                LW_IN lw_uint32_t id);
static void
lw_waiter_domain_free_global(LW_INOUT lw_waiter_domain_t *domain,
                             LW_INOUT lw_waiter_t *waiter);

typedef struct {
    lw_waiter_domain_t     lw_wgd_domain; 		/* Keep this first */
    lw_waiter_global_t     *lw_wgd_waiters[256];
    lw_uint32_t            lw_wgd_waiters_cnt; 		/* Number of allocated elements in global_waiters */
    lw_dlist_t             lw_wgd_free_list;
    pthread_key_t          lw_wgd_waiter_key; 		/* used for cleanup */
} lw_waiter_global_domain_t;

static lw_waiter_global_domain_t lw_global_waiters_domain;

lw_waiter_domain_t *lw_waiter_global_domain =
    &(lw_global_waiters_domain.lw_wgd_domain);

pthread_mutex_t lw_waiter_global_domain_lock;

#define LW_WAITERS_PER_ARRAY   (256)
#define LW_WAITERS_GLOBAL_SIZE \
    (sizeof(lw_global_waiters_domain.lw_wgd_waiters) / \
    sizeof(lw_global_waiters_domain.lw_wgd_waiters[0]))


/**
 * Allocate one array of lw_waiter_t structures.
 */
static void
lw_waiter_global_domain_alloc_one_array(LW_INOUT lw_waiter_global_domain_t *gd)
{
    lw_uint32_t i;
    lw_waiter_global_t *waiters_row;
    /*
     * TODO: need to create a wrapper around pthread_mutex_t that will be
     * called lw_pmutex_t and here we should assert that lw_waiter_global_domain_lock
     * is held by this thread
     */
    lw_verify(gd->lw_wgd_waiters_cnt < LW_WAITERS_GLOBAL_SIZE);

    gd->lw_wgd_waiters[gd->lw_wgd_waiters_cnt] =
        malloc(sizeof(lw_waiter_global_t) * LW_WAITERS_PER_ARRAY);

    waiters_row = gd->lw_wgd_waiters[gd->lw_wgd_waiters_cnt];
    for (i = 0; i < LW_WAITERS_PER_ARRAY; i++) {
        lw_waiter_global_t *gwaiter = &(waiters_row[i]);
        lw_waiter_t *waiter = &gwaiter->_waiter;
        waiter->id = gd->lw_wgd_waiters_cnt * LW_WAITERS_PER_ARRAY + i;
        waiter->initialized = FALSE;
        waiter->domain = &gd->lw_wgd_domain;

        lw_dl_init_elem(&waiter->event.iface.link);

        lw_dl_append_at_end(&gd->lw_wgd_free_list,
                            &waiter->event.iface.link);
    }
    gd->lw_wgd_waiters_cnt++;
}

void
lw_waiter_domain_init_global(lw_waiter_domain_t *domain)
{
    lw_uint32_t i;

    if (domain != NULL) {
        lw_waiter_global_domain = domain;
        return;
    }

    pthread_mutex_init(&lw_waiter_global_domain_lock, NULL);

    pthread_mutex_lock(&lw_waiter_global_domain_lock);

    lw_global_waiters_domain.lw_wgd_waiters_cnt = 0;
    lw_verify(LW_WAITERS_PER_ARRAY == 256);
    lw_dl_init(&lw_global_waiters_domain.lw_wgd_free_list);
    for (i = 0; i < LW_WAITERS_GLOBAL_SIZE; i++) {
        lw_global_waiters_domain.lw_wgd_waiters[i] = NULL;
    }
    lw_waiter_global_domain_alloc_one_array(&lw_global_waiters_domain);

    lw_verify(pthread_key_create(&lw_global_waiters_domain.lw_wgd_waiter_key,
                                 lw_waiter_free) == 0);

    lw_global_waiters_domain.lw_wgd_domain.alloc_waiter = lw_waiter_domain_alloc_global;
    lw_global_waiters_domain.lw_wgd_domain.free_waiter = lw_waiter_domain_free_global;
    lw_global_waiters_domain.lw_wgd_domain.get_waiter = lw_waiter_domain_get_global;
    lw_global_waiters_domain.lw_wgd_domain.id2waiter = lw_waiter_domain_from_id_global;
    lw_global_waiters_domain.lw_wgd_domain.waiter_event_init = lw_waiter_global_event_init;
    lw_global_waiters_domain.lw_wgd_domain.waiter_event_destroy = lw_waiter_global_event_destroy;
    lw_global_waiters_domain.lw_wgd_domain.waiter_size = sizeof(lw_waiter_t) +
                                                         sizeof(lw_thread_event_t) -
                                                         sizeof(lw_base_event_t);

    pthread_mutex_unlock(&lw_waiter_global_domain_lock);
}

/**
 * Free all the lw_waiter_t structures.
 */
static void
lw_waiter_dealloc_all(LW_INOUT lw_waiter_global_domain_t *gd)
{
    lw_uint32_t i;
    lw_delem_t *elem;
    /*
     * TODO: need to create a wrapper around pthread_mutex_t that will be
     * called lw_pmutex_t and here we should assert that lw_waiter_global_domain_lock
     * is held by this thread
     */
    lw_verify(gd->lw_wgd_waiters_cnt != 0);

    while ((elem = lw_dl_dequeue(&gd->lw_wgd_free_list)) != NULL) {
        /* Do nothing */
    }
    lw_dl_destroy(&gd->lw_wgd_free_list);

    for (i = 0; i < gd->lw_wgd_waiters_cnt; i++) {
        /*
         * All the threads that use this domain must have exited at this point.
         * If not and if they try to access their waiters, there will be trouble.
         */
        free(gd->lw_wgd_waiters[i]);
    }

    for (; i < LW_WAITERS_GLOBAL_SIZE; i++) {
        lw_verify(gd->lw_wgd_waiters[i] == NULL);
    }
    gd->lw_wgd_waiters_cnt = 0;

    lw_verify(pthread_key_delete(gd->lw_wgd_waiter_key) == 0);
}

static void
lw_waiter_clear_global(void)
{
    lw_verify(pthread_setspecific(lw_global_waiters_domain.lw_wgd_waiter_key,
                                  NULL) == 0);
}

void
lw_waiter_dealloc_global(void)
{
    lw_waiter_t *waiter = lw_waiter_get();
    lw_verify(waiter != NULL);
    lw_waiter_free(waiter);
    lw_waiter_clear_global();
}

/**
 * Shutdown the global waiter domain.
 */
void
lw_waiter_domain_shutdown_global(void)
{
    /* Free the lw_waiter_t for calling thread */
    if (lw_waiter_global_domain != &(lw_global_waiters_domain.lw_wgd_domain)) {
        /* Global domain was externally provided. Nothing more to do. */
        return;
    }
    lw_waiter_dealloc_global();
    pthread_mutex_lock(&lw_waiter_global_domain_lock);
    lw_waiter_dealloc_all(&lw_global_waiters_domain);
    pthread_mutex_unlock(&lw_waiter_global_domain_lock);
}

lw_waiter_t *
lw_waiter_domain_alloc_global(LW_INOUT lw_waiter_domain_t *domain)
{
    lw_waiter_t *waiter;
    lw_waiter_global_domain_t *gd = (lw_waiter_global_domain_t *)domain;

    pthread_mutex_lock(&lw_waiter_global_domain_lock);
    waiter = LW_FIELD_2_OBJ_NULL_SAFE(lw_dl_dequeue(&gd->lw_wgd_free_list), *waiter,
                                      event.iface.link);

    if (waiter == NULL) {
        if (gd->lw_wgd_waiters_cnt == 0) {
            /* Global domain was shutodwn already */
            pthread_mutex_unlock(&lw_waiter_global_domain_lock);
            return NULL;
        }
        lw_waiter_global_domain_alloc_one_array(gd);
        waiter = LW_FIELD_2_OBJ_NULL_SAFE(lw_dl_dequeue(&gd->lw_wgd_free_list), *waiter,
                                          event.iface.link);
    }
    pthread_mutex_unlock(&lw_waiter_global_domain_lock);

    lw_verify(waiter != NULL);
    lw_verify(waiter->initialized == FALSE);
    domain->waiter_event_init(domain, waiter);
    waiter->domain = domain;
    waiter->next = LW_WAITER_ID_MAX;
    waiter->prev = LW_WAITER_ID_MAX;
    waiter->initialized = TRUE;
    return waiter;
}

void
lw_waiter_domain_free_global(LW_INOUT lw_waiter_domain_t *domain,
                             LW_INOUT lw_waiter_t *waiter)
{
    lw_waiter_global_domain_t *gd = (lw_waiter_global_domain_t *)domain;
    pthread_mutex_lock(&lw_waiter_global_domain_lock);
    if (gd->lw_wgd_waiters_cnt > 0) { /* If this thread exits after  */
        lw_verify(waiter != NULL);
        lw_verify(waiter->event.wait_src == NULL);
        lw_verify(waiter->next == LW_WAITER_ID_MAX);

        domain->waiter_event_destroy(domain, waiter);
        waiter->initialized = FALSE;
        lw_dl_init_elem(&waiter->event.iface.link);
        lw_dl_append_at_end(&gd->lw_wgd_free_list,
                            &waiter->event.iface.link);
    }
    pthread_mutex_unlock(&lw_waiter_global_domain_lock);
}

static lw_waiter_t *
lw_waiter_domain_get_global(LW_INOUT lw_waiter_domain_t *domain)
{
    lw_waiter_t *waiter;
    lw_waiter_global_domain_t *gd = (lw_waiter_global_domain_t *)domain;
    waiter = pthread_getspecific(gd->lw_wgd_waiter_key);
    if (waiter == NULL) {
        int ret;
        waiter = domain->alloc_waiter(domain);
        ret = pthread_setspecific(gd->lw_wgd_waiter_key, waiter);
        lw_verify(ret == 0);
    }
    return waiter;
}

static lw_waiter_t *
lw_waiter_domain_from_id_global(LW_INOUT lw_waiter_domain_t *domain,
                                LW_IN lw_uint32_t id)
{
    lw_waiter_global_t *gwaiter;
    lw_waiter_t *waiter;
    lw_waiter_global_domain_t *gd = (lw_waiter_global_domain_t *)domain;

    if (id == LW_WAITER_ID_MAX) {
        return NULL;
    }

    lw_assert(id < LW_WAITER_ID_MAX);
    lw_assert((id / LW_WAITERS_PER_ARRAY) < gd->lw_wgd_waiters_cnt);
    gwaiter = &(gd->lw_wgd_waiters[id/LW_WAITERS_PER_ARRAY][id % LW_WAITERS_PER_ARRAY]);
    waiter = &gwaiter->_waiter;
    lw_assert(waiter->domain == domain);
    return waiter;
}

void
lw_waiter_remove_from_id_list(LW_INOUT lw_waiter_t *waiter)
{
    lw_waiter_t *prev;
    lw_waiter_t *next;

    next = lw_waiter_from_id(waiter->next);
    prev = lw_waiter_from_id(waiter->prev);

    if (next != NULL) {
        lw_assert(next->prev == waiter->id);
        next->prev = waiter->prev;
    }
    if (prev != NULL) {
        lw_assert(prev->next == waiter->id);
        prev->next = waiter->next;
    }
    waiter->next = waiter->prev = LW_WAITER_ID_MAX;
    waiter->event.tag = 0;
}
