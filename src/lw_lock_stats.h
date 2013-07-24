#ifndef __LW_LOCK_STATS_H__
#define __LW_LOCK_STATS_H__

#include "lw_types.h"

typedef struct lw_lock_stats_s {
    lw_uint16_t lw_lock_stats_trace_history;
    const char *lw_lock_stats_name;
    lw_atomic32_t lw_lock_stats_lock_contentions;
    lw_atomic32_t lw_lock_stats_unlock_contentions;
    lw_atomic64_t lw_lock_stats_lock_contention_cyc;
    lw_atomic64_t lw_lock_stats_unlock_contention_cyc;
} lw_lock_stats_t;

extern lw_lock_stats_t lw_lock_global_stats;


extern void
lw_lock_stats_init(INOUT lw_lock_stats_t *lw_lock_stats,
                   IN char *name);

extern lw_bool_t
lw_lock_stats_indicate_contention(IN lw_lock_stats_t *lw_lock_stats);

extern void
lw_lock_stats_reset(INOUT lw_lock_stats_t *lw_lock_stats);

extern void
lw_lock_stats_str(IN lw_lock_stats_t *lw_lock_stats,
                  char *buf,
                  size_t size);

extern void
lw_lock_stats_global_init(void);

#endif
