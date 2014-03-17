#include "lw_sync_log.h"
#include "lw_magic.h"
#include "lw_debug.h"
#include <stdlib.h>

#define LW_MAX_SYNC_LOGLINES    (4096)
#define  LW_SYNC_LOG_MAGIC   LW_MAGIC(0x5106)

struct lw_sync_log_s {
    lw_magic_t lw_sync_log_magic;
    lw_uint32_t lw_sync_log_next_line;
    lw_sync_log_line_t lw_sync_log_lines[LW_MAX_SYNC_LOGLINES];
};


lw_sync_log_t *
lw_sync_log_create(void)
{
    lw_sync_log_t *lw_sync_log = malloc(sizeof(lw_sync_log_t));
    lw_verify(lw_sync_log != NULL);
    lw_sync_log->lw_sync_log_next_line = 0;
    lw_sync_log->lw_sync_log_magic = LW_SYNC_LOG_MAGIC;

    return lw_sync_log;
}

void
lw_sync_log_destroy(lw_sync_log_t *lw_sync_log)
{
    free(lw_sync_log);
}

lw_sync_log_line_t *
lw_sync_log_next_line(lw_sync_log_t *lw_sync_log)
{
    lw_uint32_t idx;
    
    if (lw_sync_log == NULL) {
        /* The sync log feature is off for this thread. */
        return NULL;
    }

    idx = lw_sync_log->lw_sync_log_next_line++;
    lw_assert(lw_sync_log->lw_sync_log_next_line <= LW_MAX_SYNC_LOGLINES);
    if (lw_sync_log->lw_sync_log_next_line == LW_MAX_SYNC_LOGLINES) {
        lw_sync_log->lw_sync_log_next_line = 0;
    }
    lw_assert(idx < LW_MAX_SYNC_LOGLINES);
    return &lw_sync_log->lw_sync_log_lines[idx];

}
