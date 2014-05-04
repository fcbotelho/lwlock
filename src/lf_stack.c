/***
 *
 * Independently contributed to the lwlock library. This file is also released under the
 * terms of MPL 2.0 license but you do not need to sign the committer agreement for
 * this file.
 */

#include "lw_debug.h"
#include "lw_atomic.h"
#include "lf_stack.h"

#define INVALID_STACK_ELEM_ID       (0) /* Id are 1-indexed. 0 is 'NULL' */
#define STACK_ELEM_IDX_OFFSET       (1)

static lw_atomic32_t id_generator = { 0 };

void
lf_stack_init(lf_stack_t *stack,
              lw_uint8_t *base,
              lw_uint32_t num_elems,
              lw_uint64_t elem_size,
              lw_bool_t populate)
{
    lw_uint32_t i;
    stack->magic = LF_STACK_MAGIC;
    stack->id = lw_atomic32_inc_with_ret(&id_generator);
    stack->top.fields.next = INVALID_STACK_ELEM_ID;
    stack->top.fields.aba_tag = 0;
    stack->base = base;
    stack->num_elems = num_elems;
    stack->elem_size = elem_size;
    lw_verify(elem_size >= sizeof(lf_stack_elem_t));
    lw_verify(num_elems + STACK_ELEM_IDX_OFFSET <= LW_MAX_UINT32);

    if (!populate) {
        return;
    }

    for (i = 0; i < num_elems; i++) {
        lf_stack_elem_t *elem = ((lf_stack_elem_t *)base) + i;
        elem->stack_id = stack->id;
        elem->next = stack->top.fields.next;
        stack->top.fields.next = i + STACK_ELEM_IDX_OFFSET;
    }
}

static lw_uint32_t
lf_stack_obj_to_id32(lf_stack_t *stack, void *obj)
{
    lw_uint64_t offset = (lw_uint8_t *)obj - stack->base;
    lw_uint32_t idx = offset / stack->elem_size

    lw_assert(offset % stack->elem_size == 0);
    lw_assert(idx < stack->num_elems);
    return idx + STACK_ELEM_IDX_OFFSET;
}

static void *
lf_stack_id32_to_obj(lf_stack_t *stack, lw_uint32_t idx)
{
    if (idx == INVALID_STACK_ELEM_ID) {
        return NULL;
    }

    lw_assert(idx >= STACK_ELEM_IDX_OFFSET && idx < stack->num_elems + STACK_ELEM_IDX_OFFSET);
    return stack->base + (idx - STACK_ELEM_IDX_OFFSET) * stack->elem_size;
}

void
lf_stack_push(lf_stack_t *stack, void *obj)
{
    lf_stack_top_t old, new;
    lf_stack_elem_t *elem = (lf_stack_elem_t *)obj;

    lw_uint32_t idx = lf_stack_obj_to_id32(stack, obj);

    old.atomic64 = stack->top.atomic64;
    new.fields.next = idx;
    elem->stack_id = stack->id;
    do {
        new.fields.aba_tag = old.fields.aba_tag + 1;
        elem->next = old.fields.next;
    } while (!lw_uint64_swap(&stack->top.atomic64, &old.atomic64, new.atomic64));
}

void *
lf_stack_pop(lf_stack_t *stack)
{
    lf_stack_top_t old, new;
    lf_stack_elem_t *elem;

    old.atomic64 = stack->top.atomic64;
    do {
        new.fields.aba_tag = old.fields.aba_tag + 1;
        elem = lf_stack_id32_to_obj(stack, old.fields.next);
        if (elem == NULL) {
            return NULL;
        }
        new.fields.next = elem->next;
    } while (!lw_uint64_swap(&stack->top.atomic64, &old.atomic64, new.atomic64));

    elem->next = INVALID_STACK_ELEM_ID;
    return elem;
}
