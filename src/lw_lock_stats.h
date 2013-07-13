#ifndef __LW_LOCK_STATS_H__
#define __LW_LOCK_STATS_H__

#include "lw_types.h"

typedef struct lw_lock_stats_s {
    lw_int32_t lw_lock_stats_trace_id:31;
    lw_uint32_t lw_lock_stats_trace_history:1;
    lw_uint32_t lw_lock_stats_trace_gen;
    const char *lw_lock_stats_name;
    lw_atomic32_t lw_lock_stats_lock_contentions;
    lw_atomic32_t lw_lock_stats_unlock_contentions;
    lw_atomic64_t lw_lock_stats_lock_contention_cyc;
    lw_atomic64_t lw_lock_stats_unlock_contention_cyc;
} lw_lock_stats_t;

#endif
