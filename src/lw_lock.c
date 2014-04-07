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
#include "lw_cycles.h"
#include "lw_event.h"


void
lw_lock_init(lw_waiter_domain_t *domain)
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

