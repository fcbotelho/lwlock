/***
 *
 * Independently contributed to the lwlock library. This file is also released under the
 * terms of MPL 2.0 license but you do not need to sign the committer agreement for
 * this file.
 */

#ifndef __SLIST_H__
#define __SLIST_H__

#include "lw_types.h"
#include "lw_compiler.h"
#include "ptr_map.h"

/*
 * slist32 is a singly linked list with 32 bit "next pointers". The elements are assumed to come
 * from a flat array. 2 bits from the 32 bits are used for locking while the other 30 bits are the
 * array index. That supports lists with 1B elements.
 */

#define SLIST32_IDX_BITS        (30)
#define SLIST32_MAX_ELEM_IDX    ((1 << SLIST32_IDX_BITS) - 1)

typedef union {
    lw_uint32_t     atomic32;
    struct {
        lw_uint32_t lock:1;
        lw_uint32_t wait:1;
        lw_uint32_t next:SLIST32_IDX_BITS;
    } fields;
} slist32_elem_t;

LW_STATIC_ASSERT(sizeof(slist32_elem_t) == sizeof(lw_uint32_t), slist_elem_must_fit_32bits);

typedef struct {
    slist32_elem_t    head;
} slist32_t;

static INLINE void
slist32_init(slist32_t *slist)
{
    slist->head.atomic32 = 0;
}

/*
 * Raw slist interfaces. These do no locking. Use the iterator based interface below to
 * get per node locking.
 *
 * XXX: Need to have 3 bit rwlocks, so the iteration can be made more efficient. Right now
 * an iteration effectively blocks everyone behind it due its exclusive lock on the element
 * it is on.
 */
slist32_elem_t *slist_get_first(slist32_t *slist, ptr64_to32_map_t *map);
slist32_elem_t *slist_next(slist32_t *slist, slist32_elem_t *elem, ptr64_to32_map_t *map);
void slist_insert_after(slist32_t *slist,
                         slist32_elem_t *elem,
                         slist32_elem_t *new,
                         ptr64_to32_map_t *map);

slist32_elem_t *slist_pop_first(slist32_t *slist, ptr64_to32_map_t *map);
slist32_elem_t *slist_pop_next(slist32_t *slist, slist32_elem_t *elem, ptr64_to32_map_t *map);

typedef struct {
    slist32_t             *slist;
    ptr64_to32_map_t    *map;
    slist32_elem_t      *prev_elem;
    lw_bool_t           with_locks;
    lw_bool_t           next_called;
} slist_iterator_t;

void slist_iterator_init(slist_iterator_t *iter,
                         slist32_t *slist,
                         slist32_elem_t *start_after,
                         lw_bool_t with_locks,
                         ptr64_to32_map_t *map);

void slist_iterator_deinit(slist_iterator_t *iter);

slist32_elem_t *slist_iterator_next(slist_iterator_t *iter);
slist32_elem_t *slist_iterator_peek_next(slist_iterator_t *iter);
slist32_elem_t *slist_iterator_pop_next(slist_iterator_t *iter);
slist32_elem_t *slist_iterator_current(slist_iterator_t *iter);
void slist_iterator_insert(slist_iterator_t *iter, slist32_elem_t *to_insert);

#endif /* __SLIST_H__ */
