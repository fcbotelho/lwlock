/***
 * Developed originally at EMC Corporation, this library is released under the
 * MPL 2.0 license.  Please refer to the MPL-2.0 file in the repository for its
 * full description or to http://www.mozilla.org/MPL/2.0/ for the online version.
 *
 * Before contributing to the project one needs to sign the committer agreement
 * available in the "committerAgreement" directory.
 */

#ifndef __LW_LOCK_H__
#define __LW_LOCK_H__

#include "lw_types.h"
#include "lw_mutex.h"
#include "lw_mutex2b.h"
#include "lw_cond_var.h"
#include "lw_rwlock.h"
#include "lw_lock_common.h"
#include "lw_lock_stats.h"
#include "lw_thread.h"

/**
 * Initialization function
 *
 * @description
 *
 * This function should be used to initialize the lw_lock library.
 * It has to be called before using any API in the lw_lock library.
 */
extern void
lw_lock_init(lw_bool_t track_sync_events, lw_waiter_domain_t *domain);

/**
 * Cleanup function
 *
 * @description
 *
 * This function should be used to destroy/cleanup internal objects of
 * the lw_lock library. It has to be called after all the calls to
 * the APIs in the lw_lock library.
 */
extern void
lw_lock_shutdown(void);

#endif /* __LW_LOCK_H__ */
