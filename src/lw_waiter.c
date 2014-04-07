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
    lw_waiter_t            *lw_wgd_waiters[256];
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
    lw_waiter_t *waiters_row;
    /*
     * TODO: need to create a wrapper around pthread_mutex_t that will be
     * called lw_pmutex_t and here we should assert that lw_waiter_global_domain_lock
     * is held by this thread
     */
    lw_verify(gd->lw_wgd_waiters_cnt < LW_WAITERS_GLOBAL_SIZE);

    gd->lw_wgd_waiters[gd->lw_wgd_waiters_cnt] =
        malloc(sizeof(lw_waiter_t) * LW_WAITERS_PER_ARRAY);

    waiters_row = gd->lw_wgd_waiters[gd->lw_wgd_waiters_cnt];
    for (i = 0; i < LW_WAITERS_PER_ARRAY; i++) {
        lw_waiter_t *waiter = &(waiters_row[i]);
        waiter->id = gd->lw_wgd_waiters_cnt * LW_WAITERS_PER_ARRAY + i;
        waiter->initialized = FALSE;
        waiter->domain = &gd->lw_wgd_domain;

        lw_dl_init_elem(&waiter->event.base.iface.link);

        lw_dl_append_at_end(&gd->lw_wgd_free_list,
                            &waiter->event.base.iface.link);
    }
    gd->lw_wgd_waiters_cnt++;
}


void
lw_waiter_domain_init_global(lw_waiter_domain_t *domain)
{
    lw_uint32_t i;
//     lw_verify(sizeof(lw_rwlock_t) == sizeof(lw_uint32_t));

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

    pthread_mutex_unlock(&lw_waiter_global_domain_lock);
}

/**
 * Free all the lw_waiter_t structures.
 */
static void
lw_waiter_dealloc_all(LW_INOUT lw_waiter_global_domain_t *gd)
{
    lw_uint32_t i;
    lw_waiter_t *waiter;
    /*
     * TODO: need to create a wrapper around pthread_mutex_t that will be
     * called lw_pmutex_t and here we should assert that lw_waiter_global_domain_lock
     * is held by this thread
     */
    lw_verify(gd->lw_wgd_waiters_cnt != 0);

    while ((waiter = lw_dl_dequeue(&gd->lw_wgd_free_list)) != NULL) {
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
    waiter = lw_dl_dequeue(&gd->lw_wgd_free_list);
    if (waiter == NULL) {
        if (gd->lw_wgd_waiters_cnt == 0) {
            /* Global domain was shutodwn already */
            pthread_mutex_unlock(&lw_waiter_global_domain_lock);
            return NULL;
        }
        lw_waiter_global_domain_alloc_one_array(gd);
        waiter = lw_dl_dequeue(&gd->lw_wgd_free_list);
    }
    pthread_mutex_unlock(&lw_waiter_global_domain_lock);

    lw_verify(waiter != NULL);
    lw_verify(waiter->initialized == FALSE);
    lw_thread_event_init(&waiter->event);
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
        lw_verify(waiter->event.base.wait_src == NULL);
        lw_verify(waiter->next == LW_WAITER_ID_MAX);

        lw_thread_event_destroy(&waiter->event);
        waiter->initialized = FALSE;
        lw_dl_init_elem(&waiter->event.base.iface.link);
        lw_dl_append_at_end(&gd->lw_wgd_free_list,
                            &waiter->event.base.iface.link);
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
    lw_waiter_t *waiter;
    lw_waiter_global_domain_t *gd = (lw_waiter_global_domain_t *)domain;

    if (id == LW_WAITER_ID_MAX) {
        return NULL;
    }

    lw_assert(id < LW_WAITER_ID_MAX);
    lw_assert((id / LW_WAITERS_PER_ARRAY) < gd->lw_wgd_waiters_cnt);
    waiter = &(gd->lw_wgd_waiters[id/LW_WAITERS_PER_ARRAY][id % LW_WAITERS_PER_ARRAY]);
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
    waiter->event.base.tag = 0;
}
