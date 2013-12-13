#ifndef __LW_LOCK_H__
#define __LW_LOCK_H__
#include "lw_types.h"
/**
 * Initialization function
 *
 * @description
 *
 * This function should be used to initialize the lw_lock library. 
 * It has to be called before using any API in the lw_lock library.
 */
extern void 
lw_lock_init(lw_bool_t track_sync_events);

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

#include "lw_mutex.h"
#include "lw_lock_stats.h"
#include "lw_thread.h"
#endif /* __LW_LOCK_H__ */
