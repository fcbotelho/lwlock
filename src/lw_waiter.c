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
    /* Keep this first */
    lw_waiter_domain_t     lw_wgd_domain; 
    
    lw_waiter_t            *lw_wgd_waiters[256];
    
    /* Number of allocated elements in global_waiters */
    lw_uint32_t            lw_wgd_waiters_cnt; 
    
    lw_dlist_t             lw_wgd_free_list;
    
    /* used for cleanup */
    pthread_key_t          lw_wgd_waiter_key; 
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
        waiter->lw_waiter_id = gd->lw_wgd_waiters_cnt * LW_WAITERS_PER_ARRAY + i;
        waiter->lw_waiter_initialized = FALSE;
        waiter->lw_waiter_domain = &gd->lw_wgd_domain;

        lw_dl_init_elem(&waiter->lw_waiter_event.lw_te_base.lw_be_iface.lw_ei_link);

        lw_dl_append_at_end(&gd->lw_wgd_free_list, 
                            &waiter->lw_waiter_event.lw_te_base.lw_be_iface.lw_ei_link);
    }
    gd->lw_wgd_waiters_cnt++;
}


void
lw_waiter_domain_init_global(void)
{
    lw_uint32_t i;
//     lw_verify(sizeof(lw_rwlock_t) == sizeof(lw_uint32_t));

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

    lw_global_waiters_domain.lw_wgd_domain.lw_wd_alloc_waiter = 
        lw_waiter_domain_alloc_global;

    lw_global_waiters_domain.lw_wgd_domain.lw_wd_free_waiter = 
        lw_waiter_domain_free_global;

    lw_global_waiters_domain.lw_wgd_domain.lw_wd_get_waiter = 
        lw_waiter_domain_get_global;

    lw_global_waiters_domain.lw_wgd_domain.lw_wd_id2waiter = 
        lw_waiter_domain_from_id_global;

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
    lw_verify(waiter->lw_waiter_initialized == FALSE);
    lw_thread_event_init(&waiter->lw_waiter_event);
    waiter->lw_waiter_domain = domain;
    waiter->lw_waiter_next = LW_WAITER_ID_MAX;
    waiter->lw_waiter_prev = LW_WAITER_ID_MAX;
    waiter->lw_waiter_initialized = TRUE;
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
        lw_verify(waiter->lw_waiter_event.lw_te_base.lw_be_wait_src == NULL);
        lw_verify(waiter->lw_waiter_next == LW_WAITER_ID_MAX);

        lw_thread_event_destroy(&waiter->lw_waiter_event);
        waiter->lw_waiter_initialized = FALSE;
        lw_dl_init_elem(&waiter->lw_waiter_event.lw_te_base.lw_be_iface.lw_ei_link);
        lw_dl_append_at_end(&gd->lw_wgd_free_list, 
                            &waiter->lw_waiter_event.lw_te_base.lw_be_iface.lw_ei_link);
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
        waiter = domain->lw_wd_alloc_waiter(domain);
        ret = pthread_setspecific(gd->lw_wgd_waiter_key, waiter);
        lw_verify(ret != 0);
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
    lw_assert(waiter->lw_waiter_domain == domain);
    return waiter;
}

void
lw_waiter_remove_from_id_list(LW_INOUT lw_waiter_t *waiter)
{
    lw_waiter_t *prev;
    lw_waiter_t *next;

    next = lw_waiter_from_id(waiter->lw_waiter_next);
    prev = lw_waiter_from_id(waiter->lw_waiter_prev);
    
    if (next != NULL) {
        lw_assert(next->lw_waiter_prev == waiter->lw_waiter_id);
        next->lw_waiter_prev = waiter->lw_waiter_prev;
    }
    if (prev != NULL) {
        lw_assert(prev->lw_waiter_next == waiter->lw_waiter_id);
        prev->lw_waiter_next = waiter->lw_waiter_next;
    }
    waiter->lw_waiter_next = waiter->lw_waiter_prev = LW_WAITER_ID_MAX;
    waiter->lw_waiter_event.lw_te_base.lw_be_tag = 0;
}
