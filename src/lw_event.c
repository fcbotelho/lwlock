/***
 *
 * Independently contributed to the lwlock library. This file is also released under the
 * terms of MPL 2.0 license but you do not need to sign the committer agreement for
 * this file.
 */

#include "lw_atomic.h"
#include "lw_debug.h"
#include "lw_waiter.h"
#include <errno.h>

int
lw_stack_event_wait(lw_event_t event, void *arg, const struct timespec *abstime)
{
    int wait_res;
    lw_uint64_t tag_val;
    lw_base_event_t *base = LW_EVENT_2_BASE_EVENT(event);
    lw_waiter_domain_t *domain = lw_waiter_global_domain;
    lw_uint8_t  waiter_buf[domain->waiter_size];
    lw_waiter_t *waiter = (lw_waiter_t *)waiter_buf;
    lw_atomic64_t *atomic64_tag = (lw_atomic64_t *)&base->tag;

    if (lw_atomic64_read(atomic64_tag) == 1) {
        /* Done signal already received. */
        lw_atomic64_set(atomic64_tag, 0);
        return 0;
    }

    domain->waiter_event_init(domain, waiter);
    lw_waiter_set_src(waiter, base->wait_src);
    base->wait_src = waiter;
    tag_val = lw_atomic64_dec_with_ret(atomic64_tag);
    if (tag_val != 0) {
        lw_assert(tag_val == 1);
        wait_res = lw_waiter_timedwait(waiter, abstime);
        lw_assert(wait_res == 0 || wait_res == ETIMEDOUT);
        if (wait_res == ETIMEDOUT) {
            /* Increment tag again and setup for next round of possible waiting. */
            tag_val = lw_atomic64_inc_with_ret(atomic64_tag);
            lw_verify(tag_val == 1 || tag_val == 2);
            if (tag_val == 1) {
                /* Just got the signal. Consume it. */
                lw_waiter_wait(waiter);
                wait_res = 0;
                lw_atomic64_set(atomic64_tag, 0);
            }
        }
    }
    base->wait_src = waiter->event.wait_src;
    lw_waiter_clear_src(waiter);
    domain->waiter_event_destroy(domain, waiter);
    return wait_res;
}

void
lw_stack_event_signal(lw_event_t event, void *arg)
{
    lw_base_event_t *base = LW_EVENT_2_BASE_EVENT(event);
    lw_waiter_t *waiter;
    lw_atomic64_t *atomic64_tag = (lw_atomic64_t *)&base->tag;

    if (lw_atomic64_dec_with_ret(atomic64_tag) != 0) {
        /* Caller hasn't called wait yet. Nothing more to do. */
        return;
    }

    waiter = base->wait_src;
    lw_waiter_wakeup(waiter, arg);
    return;
}

lw_bool_t
lw_stack_event_wakeup_pending(lw_event_t event, void *arg)
{
    lw_base_event_t *base = LW_EVENT_2_BASE_EVENT(event);
    lw_atomic64_t *atomic64_tag = (lw_atomic64_t *)&base->tag;

    return lw_atomic64_read(atomic64_tag) == 1;
}
