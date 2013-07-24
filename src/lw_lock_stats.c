#include "lw_lock_stats.h"
#include "lw_atomic.h"
#include "lw_cycles.h"

#include <stdio.h>

lw_lock_stats_t lw_lock_global_stats;



void
lw_lock_stats_init(INOUT lw_lock_stats_t *lw_lock_stats,
                   IN char *name)
{
    lw_lock_stats->lw_lock_stats_trace_history = TRUE;
    lw_lock_stats->lw_lock_stats_name = name;
    lw_lock_stats_reset(lw_lock_stats);
}

lw_bool_t
lw_lock_stats_indicate_contention(IN lw_lock_stats_t *lw_lock_stats)
{
    lw_uint64_t lc = lw_atomic32_read(&lw_lock_stats->lw_lock_stats_lock_contentions);
    lw_uint64_t lct = lw_atomic64_read(&lw_lock_stats->lw_lock_stats_lock_contention_cyc);
    lw_uint64_t uc = lw_atomic32_read(&lw_lock_stats->lw_lock_stats_unlock_contentions);
    lw_uint64_t uct = lw_atomic64_read(&lw_lock_stats->lw_lock_stats_unlock_contention_cyc);
    return (lc != 0 || lct != 0 || uc != 0 || uct != 0);
}

void
lw_lock_stats_reset(INOUT lw_lock_stats_t *lw_lock_stats)
{
    lw_atomic32_set(&lw_lock_stats->lw_lock_stats_lock_contentions, 0);
    lw_atomic32_set(&lw_lock_stats->lw_lock_stats_unlock_contentions, 0);
    lw_atomic64_set(&lw_lock_stats->lw_lock_stats_lock_contention_cyc, 0);
    lw_atomic64_set(&lw_lock_stats->lw_lock_stats_unlock_contention_cyc, 0);
}

void
lw_lock_stats_str(IN lw_lock_stats_t *lw_lock_stats,
                  char *buf,
                  size_t size)
{
    lw_uint64_t lc = lw_atomic32_read(&lw_lock_stats->lw_lock_stats_lock_contentions);
    lw_uint64_t lct = lw_atomic64_read(&lw_lock_stats->lw_lock_stats_lock_contention_cyc);
    lw_uint64_t uc = lw_atomic32_read(&lw_lock_stats->lw_lock_stats_unlock_contentions);
    lw_uint64_t uct = lw_atomic64_read(&lw_lock_stats->lw_lock_stats_unlock_contention_cyc);

    lct = lct / lw_cpu_khz;
    uct = uct / lw_cpu_khz;

    snprintf(buf, size, "%10llu(%6llu.%03llu sec) %10llu(%6llu.%03llu sec)\n",
             lc, lct / 1000, lct % 1000,
             uc, uct / 1000, uct % 1000);
}



void
lw_lock_stats_global_init(void)
{
    lw_lock_stats_init(&lw_lock_global_stats, "lw_lock_global_stats");

}

