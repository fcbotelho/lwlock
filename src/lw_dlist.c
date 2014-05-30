/***
 * Developed originally at EMC Corporation, this library is released under the
 * MPL 2.0 license.  Please refer to the MPL-2.0 file in the repository for its
 * full description or to http://www.mozilla.org/MPL/2.0/ for the online version.
 *
 * Before contributing to the project one needs to sign the committer agreement
 * available in the "committerAgreement" directory.
 */

#include "lw_dlist.h"
#include "lw_debug.h"
#include "lw_rwlock.h"

#define ASSERT_ELEM_ON_LIST(_list, _elem)   \
    lw_assert((_elem)->list == _list && (_elem)->magic == LW_DL_ON_LIST)

#define ASSERT_ELEM_OFF_LIST(_elem)   \
    lw_assert((_elem)->list == LW_DL_DBG_BADLIST && (_elem)->magic == LW_DL_OFF_LIST)

#define LW_DL_LIST_LOCK(_list)      ((lw_rwlock_t *)&((_list)->_lock))

void
lw_dl_init(LW_INOUT lw_dlist_t *list)
{
#ifdef LW_DEBUG
    list->dl_magic = LW_DL_INITIALIZED;
#endif
    list->head = list->tail = NULL;
    list->count = 0;
    lw_rwlock_init(LW_DL_LIST_LOCK(list), LW_RWLOCK_FAIR);
}

void
lw_dl_destroy(LW_INOUT lw_dlist_t *list)
{
#ifdef LW_DEBUG
    list->dl_magic = 0;
#endif
    lw_assert(list->count == 0);
    lw_assert(list->head == list->tail);
    lw_assert(list->head == NULL);
    lw_rwlock_destroy(LW_DL_LIST_LOCK(list));
}

void
lw_dl_init_elem(LW_INOUT lw_delem_t *elem)
{
    elem->next = elem->prev = LW_DL_DBG_BADELEM;
#ifdef LW_DEBUG
    /*
     * If LW_DEBUG is set, update the list pointer and magic value so
     * that they are easily recognizable.
     */
    elem->list = LW_DL_DBG_BADLIST;
    elem->magic = LW_DL_OFF_LIST;
#endif
}

void
lw_dl_append_at_end(LW_INOUT lw_dlist_t *list,
                    LW_INOUT lw_delem_t *elem)
{
    lw_dl_insert_after(list, list->tail, elem);
}

lw_delem_t *
lw_dl_dequeue(LW_INOUT lw_dlist_t *list)
{
    lw_delem_t *elem;
    elem = list->head;

    if (elem != NULL) {
        lw_dl_remove(list, elem);
         return elem;
    }

    lw_assert(list->tail == NULL);
    lw_verify(list->count == 0);
    return NULL;
}

lw_uint32_t
lw_dl_get_count(LW_IN lw_dlist_t *list)
{
    return list->count;
}

lw_delem_t *
lw_dl_next(LW_IN lw_dlist_t *list, lw_delem_t *elem)
{
    ASSERT_ELEM_ON_LIST(list, elem);
    return elem->next;
}

lw_delem_t *
lw_dl_prev(LW_IN lw_dlist_t *list, lw_delem_t *elem)
{
    ASSERT_ELEM_ON_LIST(list, elem);
    return elem->prev;
}

void
lw_dl_insert_after(LW_INOUT lw_dlist_t *list,
                   LW_INOUT lw_delem_t *existing,
                   LW_INOUT lw_delem_t *new)
{
    ASSERT_ELEM_OFF_LIST(new);

    new->prev = existing;
    if (existing == NULL) {
        /* Assume inserting at head. */
        new->next = list->head;
        list->head = new;
    } else {
        ASSERT_ELEM_ON_LIST(list, existing);
        new->next = existing->next;
        existing->next = new;
    }

    if (existing == list->tail) {
        /* Need to update tail. */
        list->tail = new;
    }

    list->count++;

#ifdef LW_DEBUG
    new->list = list;
    new->magic = LW_DL_ON_LIST;
#endif
}

void
lw_dl_insert_before(LW_INOUT lw_dlist_t *list, lw_delem_t *existing, lw_delem_t *new)
{
    lw_dl_insert_after(list, existing->prev, new);
}

void
lw_dl_remove(LW_INOUT lw_dlist_t *list, lw_delem_t *elem)
{
    lw_delem_t *prev, *next;

    ASSERT_ELEM_ON_LIST(list, elem);
    lw_assert(list->count > 0);

    prev = elem->prev;
    next = elem->next;
    if (prev == NULL) {
        /* Elem is head of list. */
        lw_assert(elem == list->head);
        list->head = next;
    } else {
        prev->next = next;
    }

    if (next == NULL) {
        /* Elem is tail. */
        lw_assert(list->tail == elem);
        list->tail = prev;
    } else {
        next->prev = prev;
    }
    list->count--;
    lw_dl_init_elem(elem);
}

void
lw_dl_lock_writer(LW_INOUT lw_dlist_t *list)
{
    lw_rwlock_t *lock = LW_DL_LIST_LOCK(list);
    lw_rwlock_wrlock(lock);
}

void
lw_dl_unlock_writer(LW_INOUT lw_dlist_t *list)
{
    lw_rwlock_t *lock = LW_DL_LIST_LOCK(list);
    lw_rwlock_wrunlock(lock);
}

lw_int32_t
lw_dl_trylock_writer(LW_INOUT lw_dlist_t *list)
{
    lw_rwlock_t *lock = LW_DL_LIST_LOCK(list);
    return lw_rwlock_trywrlock(lock);
}

void
lw_dl_lock_reader(LW_INOUT lw_dlist_t *list)
{
    lw_rwlock_t *lock = LW_DL_LIST_LOCK(list);
    lw_rwlock_rdlock(lock);
}

void
lw_dl_unlock_reader(LW_INOUT lw_dlist_t *list)
{
    lw_rwlock_t *lock = LW_DL_LIST_LOCK(list);
    lw_rwlock_rdunlock(lock);
}

lw_int32_t
lw_dl_trylock_reader(LW_INOUT lw_dlist_t *list)
{
    lw_rwlock_t *lock = LW_DL_LIST_LOCK(list);
    return lw_rwlock_tryrdlock(lock);
}
