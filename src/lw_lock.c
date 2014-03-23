#include "lw_lock.h"
#include "lw_atomic.h"
#include "lw_cycles.h"
#include "lw_event.h"


void
lw_lock_init(lw_bool_t track_sync_events, lw_waiter_domain_t *domain)
{
    lw_atomic_init();
    lw_waiter_domain_init_global(domain);
    lw_cycles_init();
}

void
lw_lock_shutdown(void)
{
    lw_atomic_destroy();
    lw_waiter_domain_shutdown_global();
}

