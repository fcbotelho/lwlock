#include "lw_waiter.h"
#include "lw_dlist.h"
#include <pthread.h>

static lw_waiter_t *lw_waiter_alloc_global(lw_waiter_domain_t *domain);
static lw_waiter_t *lw_waiter_get_global(lw_waiter_domain_t *domain);
static lw_waiter_t *lw_waiter_from_id_global(lw_waiter_domain_t *domain,
                                             lw_uint32_t id);
static void
lw_waiter_free_global(lw_waiter_domain_t *domain, lw_waiter_t *waiter);

typedef struct {
    lw_waiter_domain_t     domain; /* Keep this first */
    lw_waiter_t            *waiters[256];
    lw_uint32_t            arrays_num; /* Number of allocated elements in global_waiters */
    lw_dlist_t             free_list;
    pthread_key_t          wait_key; /* used for cleanup */
} lw_waiter_global_domain_t;

static lw_waiter_global_domain_t   global_waiters_domain;
lw_waiter_domain_t *le_waiter_domain_global = &(global_waiters_domain.domain);
pthread_mutex_t _lw_global_lock;

/* Default stats for lwlocks. */
lw_lock_stats_t lw_lock_global_stats;


#define LW_WAITERS_PER_ARRAY   (256)
#define LW_WAITERS_GLOBAL_SIZE \
    (sizeof(global_waiters_domain.waiters) / sizeof(global_waiters_domain.waiters[0]))


/**
 * Allocate one array of dd_thread_wait_t structures.
 */
static void
lw_waiter_alloc_one_array(lw_waiter_global_domain_t *gd)
{
    lw_uint32_t i;
    lw_waiter_t *waiters_row;
    /* need to assert here that _dd_global_lock is held by this thread */
    lw_verify(gd->arrays_num < LW_WAITERS_GLOBAL_SIZE);
    gd->waiters[gd->arrays_num] = malloc(sizeof(lw_waiter_t) * LW_WAITERS_PER_ARRAY);
    waiters_row = gd->waiters[gd->arrays_num];
    for (i = 0; i < LW_WAITERS_PER_ARRAY; i++) {
        lw_waiter_t *wait = &(waiters_row[i]);
        wait->id = gd->arrays_num * LW_WAITERS_PER_ARRAY + i;
        wait->initialized = FALSE;
        wait->domain = &gd->domain;
        lw_dl_init_elem(&wait->event.base.iface.link);
        dl_append_at_end(&gd->free_list, &wait->event.base.iface.link);
    }
    gd->arrays_num++;
}


void
lw_waiter_init_global(void)
{
    lw_uint32_t i;
    lw_lock_global_stats.name = "lw_lock_global_stats";
    lw_verify(sizeof(lw_lock_t) == sizeof(lw_uint32_t));

    pthread_mutex_init(&_dd_global_lock, NULL);

    pthread_mutex_lock(&_dd_global_lock);

    global_waiters_domain.arrays_num = 0;
    lw_verify(LW_WAITERS_PER_ARRAY == 256);
    lw_dl_init(&global_waiters_domain.free_list);
    for (i = 0; i < LW_WAITERS_GLOBAL_SIZE; i++) {
        global_waiters_domain.waiters[i] = NULL;
    }
    lw_waiter_alloc_one_array(&global_waiters_domain);
    
    lw_verify(pthread_key_create(&global_waiters_domain.wait_key, lw_waiter_free) == 0);

    global_waiters_domain.domain.alloc_waiter = lw_waiter_alloc_global;
    global_waiters_domain.domain.free_waiter = lw_waiter_free_global;
    global_waiters_domain.domain.get_waiter = le_waiter_get_global;
    global_waiters_domain.domain.id2waiter = lw_waiter_from_id_global;

    pthread_mutex_unlock(&_dd_global_lock);
}

/**
 * Free all the lw_waiter_t structures.
 */
static void
lw_waiter_dealloc_all(lw_waiter_global_domain_t *gd)
{
    lw_uint32_t i;
    lw_waiter_t *wait;

    /* need to assert here that _dd_global_lock is held by this thread */

    dd_verify(gd->arrays_num != 0);

    while ((wait = dl_dequeue(&gd->free_list)) != NULL) {
        /* Do nothing */
    }
    lw_dl_destroy(&gd->free_list);

    for (i = 0; i < gd->arrays_num; i++) {
        /* Unfortunately, not all threads have exited at this point and we have
         * no choice but to deallocate the structures. The timer thread, nvl_append
         * thread and host threads are the ones that remain. If any of the remaining
         * thread tries to access its wait_t, a panic could happen. But these
         * threads don't do that.
         */
        free(gd->waiters[i]);
    }

    for (; i < LW_WAITERS_GLOBAL_SIZE; i++) {
        dd_verify(gd->waiters[i] == NULL);
    }
    gd->arrays_num = 0;

    lw_verify(pthread_key_delete(gd->wait_key) == 0);
}

static void
lw_waiter_clear_global(void)
{
    lw_verify( pthread_setspecific(global_waiters_domain.wait_key, NULL) == 0);
}

static void
lw_waiter_dealloc_global(void)
{
    lw_waiter_t *wait_tls = lw_waiter_get();
    lw_verify(wait_tls != NULL);
    lw_waiter_free(wait_tls);
    lw_waiter_clear_global();
}

/**
 * Shutdown the global waiter domain.
 */
void
lw_waiter_shutdown_global(void)
{
 
    /* Free the lw_waiter_t for calling thread */
    lw_waiter_dealloc_global();
    pthread_mutex_lock(&_dd_global_lock);
    lw_waiter_dealloc_all(&global_waiters_domain);
    pthread_mutex_unlock(&_dd_global_lock);

}

dd_thread_wait_t *
lw_waiter_alloc_global(lw_waiter_domain_t *domain)
{
    lw_waiter_t *wait;
    lw_waiter_global_domain_t *gd = (lw_waiter_global_domain_t *)domain;
    
    pthread_mutex_lock(&_dd_global_lock);
    wait = dl_dequeue(&gd->free_list);
    if (wait == NULL) {
        if (gd->arrays_num == 0) {
            /* Global domain was shutodwn already */
            pthread_mutex_unlock(&_dd_global_lock);
            return NULL;
        }
        lw_waiter_alloc_one_array(gd);
        wait = dl_dequeue(&gd->free_list);
    }
    pthread_mutex_unlock(&_dd_global_lock);

    lw_verify(wait != NULL);
    lw_verify(wait->initialized == FALSE);
    lw_event_init(&wait->event);
    wait->domain = domain;
    wait->next = LW_WAITER_ID_MAX;
    wait->prev = LW_WAITER_ID_MAX;
    wait->initialized = TRUE;
    return wait;
}

void
lw_waiter_free_global(lw_waiter_domain_t *domain, lw_waiter_t *waiter)
{
    lw_waiter_global_domain_t *gd = (lw_waiter_global_domain_t *)domain;
    pthread_mutex_lock(&_dd_global_lock);
    if (gd->arrays_num > 0) { /* If this thread exits after  */
        lw_verify(waiter != NULL);
        lw_verify(waiter->event.base.wait_src == NULL);
        lw_verify(waiter->next == DD_THREAD_WAIT_ID_MAX);

        lw_event_destroy(&waiter->event);
        waiter->initialized = FALSE;
        lw_dl_init_elem(&waiter->event.base.iface.link);
        dl_append_at_end(&gd->free_list, &waiter->event.base.iface.link);
    }
    pthread_mutex_unlock(&_dd_global_lock);
}

static lw_waiter_t *
lw_waiter_get_global(lw_waiter_domain_t *domain)
{
    lw_waiter_t *wait_tls;
    lw_waiter_global_domain_t *gd = (lw_waiter_global_domain_t *)domain;
    wait_tls = pthread_getspecific(gd->wait_key);
    if (wait_tls == NULL) {
        int ret;
        wait_tls = domain->alloc_waiter(domain);
        ret = pthread_setspecific(gd->wait_key, wait_tls);
        lw_verify(ret != 0);
    }
    return wait_tls;
}

static lw_waiter_t *
lw_waiter_from_id_global(lw_waiter_domain_t *domain, dd_uint32_t id)
{
    lw_waiter_t *waiter;
    lw_waiter_global_domain_t *gd = (lw_waiter_global_domain_t *)domain;

    if (id == LW_WAITER_ID_MAX) {
        return NULL;
    }

    lw_assert(id < LW_WAITER_ID_MAX);
    lw_assert((id / LW_WAITERS_PER_ARRAY) < gd->arrays_num);
    waiter = &(gd->waiters[id/LW_WAITERS_PER_ARRAY][id % LW_WAITERS_PER_ARRAY]);
    dd_assert(waiter->domain == domain);
    return waiter;
}
