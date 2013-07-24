#include "lw_init.h"
#include "lw_waiter.h"
#include "lw_cycles.h"
#include "lw_event.h"
#include "lw_lock_stats.h"

void
lw_init(void)
{
    lw_waiter_init_global();
    lw_cycles_init();
    lw_thread_event_init();
    lw_lock_stats_global_init();
}

void
lw_shutdown(void)
{
    lw_waiter_shutdown_global();
    lw_thread_event_destroy();
}

