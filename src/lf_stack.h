/***
 *
 * Independently contributed to the lwlock library. This file is also released under the
 * terms of MPL 2.0 license but you do not need to sign the committer agreement for
 * this file.
 */

#ifndef __LF_STACK_H__
#define __LF_STACK_H__

#include "lw_types.h"
#include "lw_magic.h"

/*
 * Lock free stack: supports push/pop without use of locking. Used for imlpementing
 * free lists of various resources.
 *
 * The current implementation assumes the elements all come from an array of fixed
 * sized structures (minimum size 8) with a maximum count that is less than 4B (ie,
 * fits in 32 bits). This is because the remaining 4 bytes of the 8 byte atomic are
 * used for the ABA tag (http://en.wikipedia.org/wiki/ABA_problem).
 *
 * The conversion from 8 byte pointer to 4 byte number (and vice-versa) is trivial for
 * an array of structs. If, in future, we have a need for a more general conversion scheme
 * (non-uniform sized objects or non-contiguous memory), we can add a pointer conversion API.
 *
 */

typedef union lf_stack_u {
    volatile lw_uint64_t atomic64;
    struct {
        lw_uint32_t next;
        lw_uint32_t aba_tag;
    } fields;
} lf_stack_top_t;

LW_STATIC_ASSERT(sizeof(lf_stack_top_t) == sizeof(lw_uint64_t), needs_64_bit_atomic);

typedef struct {
    lw_uint32_t next;
    lw_uint32_t stack_id;
} lf_stack_elem_t;

typedef struct {
    lw_magic_t      magic;
    lw_uint32_t     id;
    lf_stack_top_t  top;
    lw_uint8_t      *base;
    lw_uint32_t     num_elems;
    lw_uint64_t     elem_size;
} lf_stack_t;

void lf_stack_init(lf_stack_t *stack,
                   lw_uint8_t *base,
                   lw_uint32_t num_elems,
                   lw_uint64_t elem_size,
                   lw_bool_t populate);

void lf_stack_push(lf_stack_t *stack, void *obj);
void *lf_stack_pop(lf_stack_t *stack);

#endif /* __LF_STACK_H__ */
