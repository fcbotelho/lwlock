/***
 * Developed originally at EMC Corporation, this library is released under the
 * MPL 2.0 license.  Please refer to the MPL-2.0 file in the repository for its
 * full description or to http://www.mozilla.org/MPL/2.0/ for the online version.
 *
 * Before contributing to the project one needs to sign the committer agreement
 * available in the "committerAgreement" directory.
 */

#ifndef __ATOMIC_LIST_H__
#define __ATOMIC_LIST_H__

#include "lw_types.h"
#include "lw_debug.h"
#include "lw_util.h"
#include "lw_magic.h"
#include "lw_rwlock.h"

/*
 * Generic Linear Doubly Linked List Support with internal locking for consistency.
 *
 * This provides support for double linked lists that do not require external
 * lock for consistency. Instead each element of the list has locks that are
 * used to maintain internal consistency. It supports calls to traverse the
 * list (forward or backward), remove any specified elements, insert element
 * at any specified position etc. This enables use as a stack or a queue if
 * desired although more efficient implementations of those exist that are not
 * as heavy-weight as this.
 *
 * Note: This only provides an internally consistent list that doesn't require
 * external locking in many situations. However some use cases will still want
 * an external lock (eg: when they desire a globally consistent/frozen view). For
 * example, if iteration over all elements of the list wants a guarantee that
 * all elements (and only those elements) on the list at the start are seen,
 * then an external lock is required.
 */

/*
 * Forward reference for use within a list element structure.
 */
typedef struct atomic_dlist_struct adlist_t;
typedef struct adelem_struct adelem_t;

typedef union {
    struct {
        lw_uint16_t         pin_count:15; /* Prevent elem from being removed */
        lw_uint16_t         mask:1;       /* Set to remove elem from list */
        lw_waiter_id_t      wait_id;
    } fields;
    volatile lw_uint32_t    atomic;
} ALIGNED(4) adl_refcnt_t;

/**
 * Generic List Element structure
 */
struct adelem_struct {
    adelem_t    *next;      /**< Pointer to the next element in the list */
    adelem_t    *prev;      /**< Pointer to the previous element in the list */
    adl_refcnt_t refcnt;    /**< Ensures element is not destroyed prematurely */
    lw_rwlock_t  lock;      /**< rwlock to update prev/next pointers */
#ifdef LW_DEBUG
    adlist_t    *list;      /**< Points to the list of which it is a member */
#endif
};

/**
 * Generic List Head structure
 */
struct atomic_dlist_struct {
    adelem_t                 *head;     /**< Pointer to the first element in the list */
    adelem_t                 *tail;     /**< Pointer to the last element in the list */
    lw_rwlock_t              head_lock;
    lw_rwlock_t              tail_lock;
    lw_atomic32_t            count;    /**< Number of members in the list. Cannot be
                                            * 100 accurate without race. It is conservatively
                                            * an upper bound.
                                            */
    lw_waiter_domain_t       *domain;
    char const               *name;
#ifdef LW_DEBUG
    lw_atomic32_t            refcnt;   /**< To verify list is not destroyed prematurely */
    lw_magic_t               magic;    /**< Set to DL_INITIALIZED when initialized */
#endif
};

#define ADL_DBG_BADELEM   ((adelem_t *)0xdeadbee5)
#define ADL_DBG_BADLIST   ((adlist_t *)0xfeedfeed)

/**
 * Initialize a list, preparing it for use.
 *
 * @param list (i/o) the list to be initialized.
 */
extern void adl_init(adlist_t *const list,
                     char const *name,
                     lw_waiter_domain_t *domain);

/**
 * Destroy a list, ensuring it cannot be used again.
 *
 * Note: Since we don't keep refcnt of pending operations
 * on the list (for efficiency), it is the callers responsibility
 * to ensure that there are no pending operations on the list when the
 * destroy is called (no ongoing delete, iter, insert etc).
 *
 * @param list (i/o) the list to be destoryed.
 */
extern void adl_destroy(adlist_t *const list);

/**
 * Determine if the specified list is empty.
 *
 * @param list (i) the list to be counted.
 *
 * @return TRUE if the list is empty, FALSE otherwise.
 */
extern lw_bool_t adl_is_empty(const adlist_t *list);

/**
 * Return the number of elements in the specified list.
 *
 * @param list (i) the list to count.
 *
 * @return The number of elements in the list.
 */
extern lw_uint32_t adl_count(const adlist_t *list);

/**
 * Get the first element in a list. The returned element is pinned.
 *
 * @param list (i) the list from which the first element is
 *     requested.
 *
 * @return the first element in the list if the list is not empty,
 *    a NULL pointer otherwise.
 */
extern void *_adl_first(adlist_t *const list);

/**
 * Get the last element in a list. The returned element is pinned.
 *
 * @param list (i) the list from which the last element is
 *     requested.
 *
 * @return the last element in the list if the list is not empty,
 *    a NULL pointer otherwise.
 */
extern void *_adl_last(adlist_t *const list);

/**
 * Given an element in a list, get the next element in that list.
 * The pin on the given element is implicitly released before return.
 *
 * @param list (i) the list from which the next element is
 *     requested.
 * @param elem (i) the element for which the next pointer is
 *     requested.
 *
 * @return the next element in the list.  If the specified element is
 *     the last element in the list, return a NULL pointer.
 */
extern void *_adl_next(adlist_t *const list, adelem_t *const elem);

/**
 * Given an element in a list, get the previous element in that list.
 * The pin on the given element is implicitly released before return.
 *
 * @param list (i) the list from which the previous element is
 *     requested.
 * @param elem (i) the element for which the prev pointer is
 *     requested.
 *
 * @return the previous element in the list.  If the specified element
 *     is the first element in the list, return a NULL pointer.
 */
extern void *_adl_prev(adlist_t *const list, adelem_t *const elem);

typedef enum {
    ADL_REMOVE_FAIL = 0,            /* Elem remove failed. */
    ADL_REMOVE_SUCCESS = 1,         /* Remove was successful */
    ADL_REMOVE_SUCCESS_WAIT = 2,    /* Removed but need to call wait */
} adl_remove_result_t;

/**
 * Given a list and an element within that list, remove it. The
 * given element should already be pinned by the caller.
 *
 * @param list (i) the list from which the element is to be removed.
 * @param elem (i/o) the list element to remove.
 *
 * @return adl_remove_result_t indicating the stage of remove. FAIL indicates
 * the remove could not be done (elem could be off list or some other thread deleted
 * it already), SUCCESS indicates the element is removed, SUCCESS_WAIT indicates that
 * the caller managed to get the right to remove the element but it needs to call
 * adl_remove_elem_wait and then adl_remove_elem_do.
 *
 * In order to provide a flexible interface, the entire operation of removing
 * an element is broken down into above 3 stages. For an example use of how to
 * use all 3, see the convenience function adl_elem_delete defined below.
 * Note: if adl_remove_elem_start returns ADL_REMOVE_SUCCESS_WAIT, then one must
 * follow through later with either adl_remove_elem_wait + adl_remove_elem_do
 * or adl_remove_elem_abandon. Not doing so will leave the element on the list
 * forever as a "dead zone" (iteration will skip it, list count won't include
 * it etc).
 */
extern adl_remove_result_t adl_remove_elem_start(adlist_t *const list,
                                                 adelem_t *const elem);

/**
 * Wait for green light on removing an element. The remove attempt on the
 * element must have returned ADL_REMOVE_SUCCESS_WAIT earlier.
 *
 * @param list (i/o) the list to remove elem from.
 * @param elem (i/p) the elem to remove.
 * @param waiter (i/o) to use to perform the wait, if any. If NULL, the
 *              default waiter for the calling thread is used (based on the
 *              waiter domain set in the list).
 *
 * Note: The calling thread should release all pins on other elements of
 * the list before calling this function. Otherwise a deadlock can result.
 */
extern void adl_remove_elem_wait(adlist_t *const list,
                                 adelem_t *const elem,
                                 lw_waiter_t *waiter);

/**
 * Check if an element marked for removal is ready to be removed.
 * This must be called before calling adl_remove_elem_wait.
 */
extern lw_bool_t adl_remove_elem_ready(adlist_t *const list,
                                       adelem_t *const elem);

/**
 * Do the actual work of removing an element. The element must be masked
 * before calling this function and the caller must have done the wait already as
 * well.
 *
 * @param list (i/o) the list to remove elem from.
 * @param elem (i/o) the element to remove.
 *
 * @return None.
 */
void adl_remove_elem_do(adlist_t *const list, adelem_t *const elem);

/**
 * Convenience wrapper for the last 2 stages of removing an element as doing
 * them back to back is going to be a common use case. Note: If using
 * non-default domain for waiters, be careful about using this function. Use
 * it only if lw_thread_do_wait followed by the adl_remove_elem_do makes
 * sense.
 */
static inline void
adl_remove_elem_finish(adlist_t *const list,
                       adelem_t *const elem,
                       lw_waiter_t *waiter)
{
    lw_assert(waiter == NULL || waiter->domain == list->domain);
    adl_remove_elem_wait(list, elem, waiter);
    adl_remove_elem_do(list, elem);
}

/**
 * Remove the first element from the specified list. Do not hold
 * pins on any element before calling this.
 *
 * @param list (i) the list from which the element is to be
 *     removed.
 *
 * @return A pointer to the first element in the list if the list is
 *     not empty or a NULL pointer if the list is empty.
 */
extern void *_adl_dequeue(adlist_t *const list);

/**
 * Remove the last element from the specified list. Do not
 * hold pins on any element in the list before calling this.
 *
 * @param list (i) the list from which the element is to be
 *     removed.
 *
 * @return A pointer to the last element in the list if the list is
 *     not empty or a NULL pointer if the list is empty.
 */
extern void *_adl_pop(adlist_t *const list);

/**
 * Insert the specified element at the front of the specified list.
 *
 * @param list (i/o) the list to which the element is to be inserted.
 * @param elem (i/o) the list element to be inserted.
 * @param return_pinned (i) new will be pinned if TRUE.
 */
extern void adl_insert_at_front(adlist_t *const list,
                                adelem_t *const new,
                                LW_IN lw_bool_t return_pinned);

/**
 * Append the specified element at the end of the specified list.
 *
 * @param list (i/o) the list to which the element is to be appended.
 * @param elem (i/o) the list element to be inserted.
 * @param return_pinned (i) new will be pinned if TRUE.
 */
extern void adl_append_at_end(adlist_t *const list,
                              adelem_t *const new,
                              LW_IN lw_bool_t return_pinned);

/**
 * Insert the specified new element in front of the specified element
 * in the specified list. The elem pointer must be pinned before
 * calling the function.
 *
 * @param list (i/o) the list into which the element is to be inserted.
 * @param elem (i/o) the element before which the new element will be
 *     inserted. Must be pinned already.
 * @param new (i/o) the list element to be inserted.
 * @param return_pinned (i) new will be pinned if TRUE.
 *
 */
extern void adl_insert_elem_before(adlist_t *const list,
                                   adelem_t *const elem,
                                   adelem_t *const new,
                                   LW_IN lw_bool_t return_pinned);

/**
 * Insert the specified new element after the specified element
 * in the specified list. The elem pointer must be pinned.
 *
 * @param list (i/o) the list into which the element is to be inserted.
 * @param elem (i/o) the element after which the new element will be
 *     inserted. Must be pinned already.
 * @param new (i/o) the list element to be inserted.
 * @param return_pinned (i) new will be pinned if TRUE.
 *
 */
extern void adl_insert_elem_after(adlist_t *const list,
                                  adelem_t *const elem,
                                  adelem_t *const new,
                                  LW_IN lw_bool_t return_pinned);


#define adl_assert_elem_on_list(_list, elem) \
    lw_assert((elem) == NULL || (elem)->list == _list)

/**
 * Pin an element so it does not get deleted.
 */
lw_bool_t adl_elem_pin(adelem_t *const elem);

#define adl_assert_elem_is_pinned(elem) \
    lw_assert((elem) == NULL || (elem)->refcnt.fields.pin_count > 0)

lw_bool_t adl_elem_not_on_any_list(adelem_t const *const elem);

#define adl_assert_elem_is_pinned_or_masked(elem)                \
    lw_assert((elem) == NULL ||                                  \
              (elem)->refcnt.fields.pin_count > 0 || \
              (elem)->refcnt.fields.mask == 1)

/**
 * Unpin an element previously returned as pinned by one of the calls
 * above.
 */
void adl_elem_unpin(adlist_t *const list, adelem_t *const elem);

/**
 * Convenience wrapper for doing all the steps of removing an element.
 * Note: If using non-default domain for waiters, be careful about using
 * this function. Use it only if lw_thread_do_wait followed by the
 * adl_remove_elem_do makes sense.
 */
static inline lw_bool_t
adl_elem_delete(adlist_t *const list, adelem_t *const elem, LW_IN lw_bool_t need_pin)
{
    adl_remove_result_t res = ADL_REMOVE_SUCCESS;

    if (need_pin && !adl_elem_pin(elem)) {
        return FALSE;
    }

    res = adl_remove_elem_start(list, elem);
    switch (res) {
        case ADL_REMOVE_FAIL:
            if (need_pin) {
                adl_elem_unpin(list, elem);
            }
            return FALSE;
        case ADL_REMOVE_SUCCESS:
            return TRUE;
        case ADL_REMOVE_SUCCESS_WAIT:
            adl_remove_elem_wait(list,
                                 elem,
                                 NULL);
            adl_remove_elem_do(list, elem);
            return TRUE;
        default:
            lw_verify(FALSE);
    }
    /* Cannot get here but Windows compiler is stupid enough to want a
     * return statement here.
     */
    return FALSE;
}


typedef enum {
    ADL_ITER_FORWARD,   /* iter.next() follows next pointer */
    ADL_ITER_BACKWARD,  /* iter.next() follows prev pointer */
} adl_iter_direction_t;

typedef struct {
    adlist_t                *list;
    adelem_t                *current_elem;    /* iter is at this element */
    lw_uint32_t             count;            /* number of elements iterated */
    adl_iter_direction_t    direction;
    lw_bool_t               return_current;   /* Set after iter_pop as the iterator
                                               * internally advanced but the element
                                               * is not consumed yet.
                                               */
} adlist_iter_t;

/* Initialize an adlist iter */
extern void adlist_iter_init(adlist_iter_t *const iter,
                             adlist_t *const list,
                             LW_IN adl_iter_direction_t direction);

/**
 * Return next element in iteration. Return NULL if done. The lock on the
 * element returned in previous call to this function will be implicitly
 * released.
 */
extern void *_adlist_iter_next(adlist_iter_t *const iter);

/**
 * Pop the current element of the iterator off the list. The return value
 * indicates if the pop was successful or not. Upon success, the iterator
 * also advances, internally, to the next element that will be returned on
 * the next invocation of adlist_iter_next. Upon failure, there is no need
 * to internally advance the iterator. The caller doesn't need to worry
 * about this detail.
 *
 * The pop operation is in 2 stages. The start function will do the masking
 * of the element and the setup needed. The finish operation will complete
 * the delete and advancing of iter.
 */
extern lw_bool_t adlist_iter_pop_start(adlist_iter_t *const iter);
extern void adlist_iter_pop_finish(adlist_iter_t *const iter);

static inline lw_bool_t
adlist_iter_pop(adlist_iter_t *const iter)
{
    if (adlist_iter_pop_start(iter)) {
        adlist_iter_pop_finish(iter);
        return TRUE;
    }
    return FALSE;
}

/**
 * Set the "current" element of the iterator to the given element.
 * adlist_iter_next will return the element after it. Specify
 * need_pin to be TRUE if the given elem is unpinned currently.
 */
extern lw_bool_t adlist_iter_seek(adlist_iter_t *const iter,
                                  adelem_t *const elem,
                                  lw_bool_t need_pin);

extern void adlist_iter_destroy(adlist_iter_t *const iter);

#define _ADL_ITEM_FROM_LINK(_linkp, _type, _member) LW_FIELD_2_OBJ_NULL_SAFE(_linkp, _type, _member)

#define ADL_NEXT(_listp, _itemp, _type, _member)                   \
    _ADL_ITEM_FROM_LINK(_adl_next(_listp, &((_itemp)->_member)), _type, _member)

#define ADL_PREV(_listp, _itemp, _type, _member)                 \
    _ADL_ITEM_FROM_LINK(_adl_prev(_listp, &((_itemp)->_member)), _type, _member)

#define ADL_FIRST(_listp, _type, _member) \
    _ADL_ITEM_FROM_LINK(_adl_first((_listp)), _type, _member)

#define ADL_LAST(_listp, _type, _member) \
    _ADL_ITEM_FROM_LINK(_adl_last((_listp)), _type, _member)

#define ADL_DEQUEUE(_listp, _type, _member) \
    _ADL_ITEM_FROM_LINK(_adl_dequeue((_listp)), _type, _member)

#define ADL_POP(_listp, _type, _member) \
    _ADL_ITEM_FROM_LINK(_adl_pop((_listp)), _type, _member)

#define ADL_ITER_NEXT(_iterp, _type, _member) \
    _ADL_ITEM_FROM_LINK(_adlist_iter_next((_iterp)), _type, _member)

#endif /* __ATOMIC_LIST_H__ */
