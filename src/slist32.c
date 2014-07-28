/***
 *
 * Independently contributed to the lwlock library. This file is also released under the
 * terms of MPL 2.0 license but you do not need to sign the committer agreement for
 * this file.
 */

#include "slist32.h"
#include "lw_atomic.h"
#include "lw_bitlock.h"

slist32_elem_t *
slist_get_first(slist32_t *slist, ptr64_to32_map_t *map)
{
    return map->id2obj(map, slist->head.fields.next);
}

slist32_elem_t *
slist_next(slist32_t *slist, slist32_elem_t *elem, ptr64_to32_map_t *map)
{
    if (elem == NULL) {
        /* Interpret as get first. */
        return slist_get_first(slist, map);
    }
    return map->id2obj(map, elem->fields.next);
}

void
slist_insert_after(slist32_t *slist,
                   slist32_elem_t *elem,
                   slist32_elem_t *new,
                   ptr64_to32_map_t *map)
{
    lw_uint32_t idx = map->obj2id(map, new);

    lw_assert(idx <= SLIST32_MAX_ELEM_IDX);
    elem->fields.next = idx;
}

slist32_elem_t *
slist_pop(slist32_t *slist, ptr64_to32_map_t *map)
{
    slist32_elem_t *elem = slist_get_first(slist, map);
    if (elem != NULL) {
        slist->head = *elem;
    }
    return elem;
}

slist32_elem_t *
slist_pop_next(slist32_t *slist, slist32_elem_t *elem, ptr64_to32_map_t *map)
{
    slist32_elem_t *popped = slist_next(slist, elem, map);
    if (popped != NULL) {
        *elem = *popped;
    }
    return popped;
}

static slist32_elem_t lock_mask = { .fields = { .lock = 1, .wait = 0, .next = 0 } };
static slist32_elem_t wait_mask = { .fields = { .lock = 0, .wait = 1, .next = 0 } };

static void
lock_elem(slist32_elem_t *elem)
{
    lw_bitlock32_lock(&elem->atomic32, lock_mask.atomic32, wait_mask.atomic32);
}

static void
unlock_elem(slist32_elem_t *elem)
{
    lw_bitlock32_unlock(&elem->atomic32, lock_mask.atomic32, wait_mask.atomic32);
}

static void
set_next_locked(slist32_elem_t *elem, lw_uint32_t next)
{
    slist32_elem_t old, new;

    lw_assert(elem->fields.lock == 1);
    lw_assert(next <= SLIST32_MAX_ELEM_IDX);

    old.atomic32 = elem->atomic32;
    do {
        new.atomic32 = old.atomic32;
        new.fields.next = next;
    } while (!lw_uint32_swap(&elem->atomic32, &old.atomic32, new.atomic32));
}

/**
 * Initialize an iterator over an slist. If use_locks is true, then each element will be indvidually
 * locked during iteration allowing for concurrent threads to operate on the list.
 *
 * If the start_after elem is not NULL, then iteration will return the element after it. The caller
 * is responsible for ensuring that it is actually on the list (and remains so) during the call. It will
 * be locked if locking mode is specified.
 */
void
slist_iterator_init(slist_iterator_t *iter,
                    slist32_t *slist,
                    slist32_elem_t *start_after,
                    lw_bool_t with_locks,
                    ptr64_to32_map_t *map)
{
    iter->slist = slist;
    iter->map = map;
    iter->prev_elem = start_after != NULL ? start_after : &slist->head;
    iter->with_locks = with_locks;
    iter->next_called = FALSE;
    if (with_locks) {
        lock_elem(iter->prev_elem);
    }
}

void
slist_iterator_deinit(slist_iterator_t *iter)
{
    if (iter->with_locks && iter->prev_elem != NULL) {
        /* Unlock elem, if any. */
        unlock_elem(iter->prev_elem);
    }
}

/**
 * Peek at the next element. It is not yet locked so deferencing it should be limited by caller to safe
 * fields. Reading the next of next should be espcially avoided in general unless special use case allows
 * caller to reason it is safe.
 */
slist32_elem_t *
slist_iterator_peek_next(slist_iterator_t *iter)
{
    if (iter->prev_elem == NULL) {
        return NULL;
    }
    return slist_next(iter->slist, iter->prev_elem, iter->map);
}

/**
 * Return next element and move the iterator forward. The next element is locked, and lock on prev
 * elem is dropped.
 */
slist32_elem_t *
slist_iterator_next(slist_iterator_t *iter)
{
    slist32_elem_t *next;

    if (iter->prev_elem == NULL) {
        lw_assert(iter->next_called);
        return NULL;
    }

    next = slist_iterator_peek_next(iter);

    if (iter->with_locks) {
        if (next != NULL) {
            lock_elem(next);
        }
        unlock_elem(iter->prev_elem);
    }
    iter->prev_elem = next;
    iter->next_called = TRUE;
    return next;
}

slist32_elem_t *
slist_iterator_current(slist_iterator_t *iter)
{
    lw_assert(iter->next_called);
    lw_assert(iter->prev_elem != &iter->slist->head);
    return iter->prev_elem;
}

slist32_elem_t *
slist_iterator_pop_next(slist_iterator_t *iter)
{
    slist32_elem_t *next;
    lw_assert(iter->prev_elem != NULL);

    next = slist_iterator_peek_next(iter);
    if (next == NULL)  {
        return NULL;
    }

    if (iter->with_locks) {
        lock_elem(next);
        set_next_locked(iter->prev_elem, next->fields.next);
        unlock_elem(next);
    } else {
        *iter->prev_elem = *next;
    }
    return next;
}

void
slist_iterator_insert(slist_iterator_t *iter, slist32_elem_t *to_insert)
{
    lw_uint32_t idx = iter->map->obj2id(iter->map, to_insert);

    lw_assert(idx <= SLIST32_MAX_ELEM_IDX);

    if (iter->with_locks) {
        set_next_locked(iter->prev_elem, idx);
    } else {
        iter->prev_elem->fields.next = idx;
    }
}
