/***
 *
 * Independently contributed to the lwlock library. This file is also released under the
 * terms of MPL 2.0 license but you do not need to sign the committer agreement for
 * this file.
 */

#include "lw_debug.h"
#include "counter.h"
#include <errno.h>

void
counter_init(counter_t *counter, lw_uint32_t max, lw_bool_t hard_max)
{
    delegate_init(&counter->delegate);
    counter_state_t *state = (counter_state_t *)&counter->delegate.state;
    state->fields.available = max;
    counter->hard_max = hard_max;
    counter->max = max;
}

void
counter_deinit(counter_t *counter)
{
    lw_verify(!counter->hard_max ||
              counter_available(counter) == counter->max);
    delegate_deinit(&counter->delegate);
}

/**
 * Internal function that does the compare exchange work for allocation. It can be invoked
 * in the direct caller path (fair will be set to TRUE) or via delegated jobs (where fair will
 * be FALSE to ignore the delegate internal bits).
 */
static lw_bool_t
counter_reserve_work(counter_t *counter, lw_uint32_t need, lw_bool_t fair)
{
    counter_state_t old, new;
    counter_state_t *state = (counter_state_t *)&counter->delegate.state;

    old.atomic64 = state->atomic64;
    do {
        if (old.fields.available < need ||
            (fair && (old.fields.lock | old.fields.incoming | old.fields.pending))) {
            /* Don't have available units or have other folks waiting. */
            return FALSE;
        }
        new.atomic64 = old.atomic64;
        new.fields.available -= need;
    } while (!lw_uint64_swap(&state->atomic64, &old.atomic64, new.atomic64));

    return TRUE;
}

static delegate_job_status_t
counter_alloc_job(delegate_t *delegate, void *arg)
{
    counter_async_alloc_ctx_t *ctx = (counter_async_alloc_ctx_t *)arg;
    counter_t *counter = LW_FIELD_2_OBJ(delegate, *counter, delegate);

    if (counter_reserve_work(counter, ctx->need, FALSE)) {
        /* All done. */
        return DELEGATED_DONE;
    }
    return DELEGATED_BLOCK_ALL;
}

static void
counter_async_alloc_ctx_init(counter_async_alloc_ctx_t *ctx, lw_uint32_t need)
{
    ctx->need = need;
    ctx->alloc_job.func = counter_alloc_job;
    ctx->alloc_job.arg = ctx;
    delegate_job_event_default_init(&ctx->alloc_job);
}

lw_bool_t
counter_try_alloc(counter_t *counter, lw_uint32_t need)
{
    return counter_reserve_work(counter, need, TRUE);
}

/**
 * Returns TRUE is allocation was done inline, FALSE if queued and wait
 * needs to be called later.
 */
lw_bool_t
counter_alloc_async(counter_t *counter, lw_uint32_t need,
                    counter_async_alloc_ctx_t *ctx)
{

    if (counter_try_alloc(counter, need)) {
        return TRUE;
    }

    counter_async_alloc_ctx_init(ctx, need);
    delegate_submit(&counter->delegate, &ctx->alloc_job, TRUE);
    return FALSE;
}

void
counter_alloc(counter_t *counter, lw_uint32_t need)
{
    counter_async_alloc_ctx_t ctx;

    if (counter_alloc_async(counter, need, &ctx)) {
        return;
    }
    counter_async_alloc_finish_wait(counter, &ctx);
}

void
counter_free(counter_t *counter, lw_uint32_t count)
{
    counter_state_t old, new;
    counter_state_t *state = (counter_state_t *)&counter->delegate.state;

    lw_assert(count != 0);

    old.atomic64 = state->atomic64;
    do {
        new.atomic64 = old.atomic64;
        new.fields.available += count;
        if (old.fields.incoming | old.fields.pending) {
            new.fields.lock = 1;
        }
    } while (!lw_uint64_swap(&state->atomic64, &old.atomic64, new.atomic64));

    lw_assert(new.fields.available != 0);
    lw_verify(new.fields.available <= counter->max || !counter->hard_max);

    if (old.fields.lock == 0 && new.fields.lock == 1) {
        /* Could have just XOR-ed them as well but above is clearer in intent. */
        delegate_run_internal(&counter->delegate);
    }
}

lw_uint32_t
counter_available(counter_t *counter)
{
    counter_state_t old;
    counter_state_t *state = (counter_state_t *)&counter->delegate.state;

    old.atomic64 = state->atomic64;
    return old.fields.available;
}

lw_bool_t
counter_alloc_async_cancel(counter_t *counter, counter_async_alloc_ctx_t *ctx)
{
    int cancel_result = delegate_cancel(&counter->delegate, &ctx->alloc_job, TRUE);
    lw_assert(cancel_result == 0 || cancel_result == EALREADY);
    return cancel_result == 0;
}
