#include "lw_lock.h"
#include "lw_atomic.h"
#include "lw_cycles.h"
#include "lw_waiter.h"
#include "lw_event.h"
#include "lw_lock_stats.h"
#include "lw_sync_log.h"


void lw_lock_init(void)
{
    lw_atomic_init();
    lw_waiter_init_global();
    lw_cycles_init();
    lw_lock_stats_global_init();
    lw_sync_log_init();
}

void lw_lock_shutdown(void)
{
    lw_atomic_destroy();
    lw_waiter_shutdown_global();
    lw_sync_log_shutdown();
}

