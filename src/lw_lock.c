/***
 * Developed originally at EMC Corporation, this library is released under the
 * MPL 2.0 license.  Please refer to the MPL-2.0 file in the repository for its
 * full description or to http://www.mozilla.org/MPL/2.0/ for the online version.
 *
 * Before contributing to the project one needs to sign the committer agreement
 * available in the "committerAgreement" directory.
 */

#include "lw_lock.h"
#include "lw_atomic.h"
#include "lw_bitlock.h"
#include "lw_cycles.h"
#include "lw_event.h"
#include "lw_monitor.h"


void
lw_lock_init(lw_waiter_domain_t *domain,
             lw_uint32_t bitlock_lists_count,
             void *bitlock_wait_list_memory,
             lw_uint32_t num_monitors)
{
    lw_atomic_init();
    lw_waiter_domain_init_global(domain);
    lw_cycles_init();
    lw_bitlock_module_init(bitlock_lists_count, bitlock_wait_list_memory);
    lw_monitor_module_init(num_monitors);
}

void
lw_lock_shutdown(void)
{
    lw_bitlock_module_deinit();
    lw_atomic_destroy();
    lw_waiter_domain_shutdown_global();
    lw_monitor_module_deinit();
}

