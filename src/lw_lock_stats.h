#ifndef __LW_LOCK_STATS_H__
#define __LW_LOCK_STATS_H__

#include "lw_types.h"
#include <stddef.h>

typedef struct lw_lock_stats_s {
    lw_bool_t lw_ls_trace_history; /* whether or not to use sync log */
    const char *lw_ls_name;
    lw_atomic32_t lw_ls_lock_contentions;
    lw_atomic32_t lw_ls_unlock_contentions;
    lw_atomic64_t lw_ls_lock_contention_cyc;
    lw_atomic64_t lw_ls_unlock_contention_cyc;
} lw_lock_stats_t;

extern void
lw_lock_stats_init(LW_INOUT lw_lock_stats_t *lw_lock_stats,
                   LW_IN char *name);

extern lw_bool_t
lw_lock_stats_indicate_contention(LW_IN lw_lock_stats_t *lw_lock_stats);

extern void
lw_lock_stats_reset(LW_INOUT lw_lock_stats_t *lw_lock_stats);

extern void
lw_lock_stats_str(LW_IN lw_lock_stats_t *lw_lock_stats,
                  LW_OUT char *buf,
                  LW_IN size_t size);

extern void
lw_lock_stats_global_init(void);

extern lw_lock_stats_t *
lw_lock_stats_get_global(void);

#endif
