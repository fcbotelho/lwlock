#include "lw_lock.h"
#include "lw_atomic.h"
#include "lw_cycles.h"
#include "lw_event.h"
#include "lw_lock_stats.h"
#include "lw_thread.h"


void
lw_lock_init(lw_bool_t track_sync_events, lw_waiter_domain_t *domain)
{
    lw_atomic_init();
    lw_waiter_domain_init_global(domain);
    lw_cycles_init();
    lw_lock_stats_global_init();
    lw_thread_system_init(track_sync_events);
}

void
lw_lock_shutdown(void)
{
    lw_atomic_destroy();
    lw_waiter_domain_shutdown_global();
    lw_thread_system_shutdown();
}

