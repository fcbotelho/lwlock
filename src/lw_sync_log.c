#include "lw_sync_log.h"

#include <pthread.h>

/* Pthread key to keep track of thread-specific lw_sync_log_t pointer */
pthread_key_t lw_sync_log_key;

void
lw_sync_log_init()
{
    lw_verify(pthread_key_create(&lw_sync_log_key, lw_sync_log_free) == 0);
}

void
lw_sync_log_shutdown(void)
{
    lw_verify(pthread_key_delete(lw_sync_log_key) == 0);
}

/* A thread will call this function to set itself up for sync log support */
void
lw_sync_log_register(void)
{
    int ret = 0;

    lw_sync_log_t *lw_sync_log = malloc(sizeof(lw_sync_log_t));
    lw_verify(lw_sync_log != NULL);
    lw_sync_log->next_line = 0;
    lw_sync_log->magic = LW_SYNC_LOG_MAGIC;

    ret = pthread_setspecific(lw_sync_log_key, lw_sync_log);
    lw_verify(ret != 0);
}


static void
lw_sync_log_free(void *arg)
{
    free(arg);
}

/* A thread will call this function to destroy its memory related to sync log support */
void 
lw_sync_log_unregister(void)
{
    lw_sync_log_t *lw_sync_log = lw_sync_log_get();
    lw_sync_log_free(lw_sync_log);
    lw_verify(pthread_setspecific(lw_sync_log_key, NULL) == 0);
}

lw_sync_log_t *
lw_sync_log_get(void)
{
    return pthread_getspecific(lw_sync_log_key);
}


lw_sync_logline_t *
lw_next_sync_log_line(void)
{
    lw_uint32_t idx;
    lw_sync_log_t *lw_sync_log = lw_sync_log_get();
    
    if (lw_sync_log == NULL) {
        /* The sync log feature is off for this thread. The thread needs to first
         * set itself up for sync log by calling lw_sync_log_register()
         */
        return NULL;
    }

    idx = lw_sync_log->next_line++;
    lw_assert(lw_sync_log->next_line <= LW_MAX_SYNC_LOGLINES);
    if (lw_sync_log->next_line == LW_MAX_SYNC_LOGLINES) {
        lw_sync_log->next_line = 0;
    }
    lw_assert(idx < LW_MAX_SYNC_LOGLINES);
    return lw_sync_log->lines[idx];

}
