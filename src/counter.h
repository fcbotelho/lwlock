/***
 *
 * Independently contributed to the lwlock library. This file is also released under the
 * terms of MPL 2.0 license but you do not need to sign the committer agreement for
 * this file.
 */

#ifndef __COUNTER_H__
#define __COUNTER_H__

#include "delegate.h"
#include "lw_atomic.h"
#include "lw_waiter.h"

/*
 * Counter is a safe atomic abstraction that is similar to semaphores for managing resources.
 * It is more general than a semaphore as it targets the use case of allocating/freeing resources
 * as the primary use case.
 *
 * The counter implementation is strictly fair and assumes all callers have the same priority. Adding
 * priority or water-mark based reservations can be added too but adds non-trivial complexity to the code
 * and it is often possible to achieve the same goal by using a combination of counters.
 */

#define DELEGATE_OTHER_FIELDS                               \
    lw_uint64_t     available:32;                           \
    lw_uint64_t     unused:28;

#define DELEGATE_STATE_TYPE_NAME    counter_state_t

#include "delegate_state_gen.h"

#undef DELEGATE_OTHER_FIELDS
#undef DELEGATE_STATE_TYPE_NAME

typedef struct {
    delegate_t  delegate;
    lw_uint32_t max;
    lw_bool_t   hard_max;
} counter_t;

typedef struct {
    delegated_job_t alloc_job;
    lw_uint32_t     need;
    lw_atomic32_t   done_countdown;
    lw_waiter_t     *waiter;
} counter_async_alloc_ctx_t;

void counter_init(counter_t *counter, lw_uint32_t max, lw_bool_t hard_max);
void counter_deinit(counter_t *counter);
void counter_alloc(counter_t *counter, lw_uint32_t need);
lw_bool_t counter_try_alloc(counter_t *counter, lw_uint32_t need);
void counter_free(counter_t *counter, lw_uint32_t count);
lw_uint32_t counter_available(counter_t *counter);

lw_bool_t counter_alloc_async(counter_t *counter, lw_uint32_t need,
                              counter_async_alloc_ctx_t *ctx);

lw_bool_t counter_alloc_async_cancel(counter_t *counter, counter_async_alloc_ctx_t *ctx);

static INLINE lw_bool_t
counter_alloc_is_done(counter_t *counter, counter_async_alloc_ctx_t *ctx)
{
    return lw_event_wakeup_pending(&ctx->alloc_job.event, &counter->delegate);
}

static INLINE void
counter_async_alloc_finish_wait(counter_t *counter, counter_async_alloc_ctx_t *ctx)
{
    lw_event_wait(&ctx->alloc_job.event, &counter->delegate);
    lw_assert(lw_atomic32_read(&ctx->done_countdown) == 0);
}

#endif /* __COUNTER_H__ */
