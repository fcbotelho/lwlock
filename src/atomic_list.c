/***
 * Developed originally at EMC Corporation, this library is released under the
 * MPL 2.0 license.  Please refer to the MPL-2.0 file in the repository for its
 * full description or to http://www.mozilla.org/MPL/2.0/ for the online version.
 *
 * Before contributing to the project one needs to sign the committer agreement
 * available in the "committerAgreement" directory.
 */

#include "atomic_list.h"
#include "lw_atomic.h"
#include <errno.h>

/**
 * Implementation of internally consistent list. No external locking is needed for the list
 * operations supported: next, prev, insert, enq, deq etc. The interfaces are not without
 * restrictions however: An element must be pinned before being removed. A destory must not
 * be called if some API operation is in progress, etc.
 *
 * TODO/XXX: The following is the list of items that reamin to be done to enhance the usefulness
 * of this construct. Remove items from the list as and when they get done.
 * c) Enhance iterator abstraction to work for both adlist and dlist.
 * d) Change head and tail pointer of list into adelem_t so the code doesn't need to have explicit
 *    checks in functions for prev/next pointer being NULL. We will still need checks to ensure we
 *    don't return the "list elements" during iteration etc.
 * f) Possible testing idea: do random operations on adlist and a dlist. Ensure they are consistent.
 * g) Provide a remove if not pinned call to compensate for the fact that abandon is currently disabled.
 * i) ZVM could have issues. Look into it.
 */

/*
 * Note for implementors: If you are planning to change/enhance/extend/add APIs to atomic lists, here
 * are some guidelines you might find useful.
 * -- Do not read the prev/next pointer of any element without locking it first. Do not access the fields
 * directly. Always use the ADL_ELEM_NEXT/ADL_ELEM_PREV etc macros which in debug build check the locking
 * requirement. Conversely, do not update the prev/next pointer of an element without taking exclusive lock
 * on it first.
 * -- The "natural" locking order is along the "next" pointer of elements. A blocking lock can be done when
 * locking the "next" element. Otherwise the deferred blocking (async) lock must be used. When taking async lock,
 * the starting element's lock needs to be dropped and re-acquired. Hence, always recheck the elem and
 * prev_elem relation to ensure it hasn't changed. If it has, that usually requires "restarting from the
 * top".
 * -- To insert or remove an element, the element and both its neighbours must be exclusively locked before
 * the pointers are updated. This ensures that we always see a consistent image of the element and its neighbours.
 * -- Do not try any operation on an element unless you are sure it is still valid and in the list. There
 * are multiple ways of ensuring this. It usually starts by having the element pinned (an incoming
 * requirement for most of the API calls) and moving to other mechanism in the lifecycle of the API call.
 * -- The following ways ensure an element is going to remain on list:
 *  # The element is pinned. Most API calls start here. This ensures element won't get removed (but can be
 *  marked to) from the list as the remove operation will wait for unpin.
 *  # The element lock is taken (either shared or exclusive). This prevents update of its prev/next pointer
 *  as well as the update of the pointers of its neighbour that point back to it (prev of next and next of
 *  prev).
 *  # The element's neighbour's lock is taken. This prevents update of the neighbour pointer pointing to the
 *  element. Don't overlook the usefulness of this point.
 * -- A typical operation will start with an element pinned, will lock it (in shared mode) and then try to
 * do the same to the interesting neighbours. A careful stepping ensures the element will remain valid
 * until (due to one of the constraints listed above being followed).
 * -- Treat a masked element as not being present in the list. It is marked for delete and should only be
 * used to step across to get to other elements.
 */

#ifdef LW_DEBUG
#define ADL_ELEM_NEXT(elem)     (((elem)->lock.locked) ? (elem)->next : ADL_DBG_BADELEM + 5)
#define ADL_ELEM_PREV(elem)     (((elem)->lock.locked) ? (elem)->prev : ADL_DBG_BADELEM + 5)
#define ADL_LIST_HEAD(list)     (((list)->head_lock.locked) ? (list)->head : ADL_DBG_BADELEM + 5)
#define ADL_LIST_TAIL(list)     (((list)->tail_lock.locked) ? (list)->tail : ADL_DBG_BADELEM + 5)
#else
#define ADL_ELEM_NEXT(elem)     ((elem)->next)
#define ADL_ELEM_PREV(elem)     ((elem)->prev)
#define ADL_LIST_HEAD(list)     ((list)->head)
#define ADL_LIST_TAIL(list)     ((list)->tail)
#endif
#define ADL_ELEM_NEXT_SET(elem, val) \
        do { \
            lw_assert((elem)->lock.wlocked != 0); \
            (elem)->next = val; \
        } while (0)
#define ADL_ELEM_PREV_SET(elem, val) \
        do { \
            lw_assert((elem)->lock.wlocked != 0); \
            (elem)->prev = val; \
        } while (0)
#define ADL_LIST_HEAD_SET(list, val) \
        do { \
            lw_assert((list)->head_lock.wlocked != 0); \
            (list)->head = val; \
        } while (0)
#define ADL_LIST_TAIL_SET(list, val) \
        do { \
            lw_assert((list)->tail_lock.wlocked != 0); \
            (list)->tail = val; \
        } while (0)

typedef enum {
    ADL_LOCK_SHARED = LW_RWLOCK_SHARED,
    ADL_LOCK_SYNC = LW_RWLOCK_WAIT_INLINE,
    ADL_LOCK_EXCLUSIVE = LW_RWLOCK_EXCLUSIVE,
    ADL_LOCK_ASYNC = LW_RWLOCK_WAIT_DEFERRED,
} adl_lock_attempt_t;

static int
adl_lock(adlist_t *const list,
         lw_rwlock_t *const lock,
         LW_IN adl_lock_attempt_t try_type)
{
    int ret;
    lw_waiter_t *waiter = NULL;

    if ((try_type & ADL_LOCK_ASYNC) == ADL_LOCK_ASYNC) {
        waiter = lw_waiter_get();
    }

    ret = lw_rwlock_lock(lock, try_type | LW_RWLOCK_WAIT, waiter);
    lw_assert(ret == 0 || ret == EWOULDBLOCK);
    return ret;
}

/**
 * Unlock an element previous returned as locked by one of the calls
 * above.
 */
static void
adl_unlock(adlist_t *const list,
           lw_rwlock_t *const lock,
           LW_IN adl_lock_attempt_t try_type)
{
    lw_rwlock_unlock(lock,
                     (try_type & ADL_LOCK_EXCLUSIVE) == ADL_LOCK_EXCLUSIVE);
}

static inline void
adl_lock_sync(adlist_t *const list,
              lw_rwlock_t *const lock,
              LW_IN adl_lock_attempt_t try_type)
{
    int ret;
    lw_assert(!(try_type & ADL_LOCK_ASYNC));
    ret = adl_lock(list, lock, try_type | ADL_LOCK_SYNC);
    LW_UNUSED_PARAMETER(ret); // For non-debug builds.
    lw_assert(ret == 0);
}

static inline int
adl_lock_async(adlist_t *const list,
               lw_rwlock_t *const lock,
               LW_IN adl_lock_attempt_t try_type)
{
    return adl_lock(list, lock, try_type | ADL_LOCK_ASYNC);
}

static void
adl_lock_wait(adlist_t *const list,
              lw_rwlock_t *const lock,
              LW_IN adl_lock_attempt_t try_type)
{

    lw_rwlock_contention_wait(lock,
                              try_type,
                              lw_waiter_get());
}

/**
 * Pin an element so it does not get deleted.
 */
lw_bool_t
adl_elem_pin(adelem_t *const elem)
{
    adl_refcnt_t old, new;

    lw_assert(elem != NULL);
    old.atomic = elem->refcnt.atomic;
    do {
        new.atomic = old.atomic;
        if (old.fields.mask) {
            return FALSE;
        }
        new.fields.pin_count += 1;
    } while (!lw_uint32_swap(&elem->refcnt.atomic, &old.atomic, new.atomic));

    lw_verify(new.fields.pin_count != 0); /* no overflow */
    return TRUE;
}

/**
 * Unpin an element previously returned as pinned by one of the calls
 * above.
 */
void
adl_elem_unpin(adlist_t *const list,
               adelem_t *const elem)
{
    adl_refcnt_t old, new;
    lw_bool_t do_signal = FALSE;

    lw_assert(list != NULL && elem != NULL);
    lw_assert(lw_atomic32_inc_with_ret(&list->refcnt) > 0);
    adl_assert_elem_on_list(list, elem);
    old.atomic = elem->refcnt.atomic;
    do {
        new.atomic = old.atomic;
        lw_assert(old.fields.pin_count > 0);
        new.fields.pin_count -= 1;
        if (old.fields.mask == 1 &&
            new.fields.pin_count == 0) {
            /* A delete on this element is requested and is now
             * ready to proceed.
             */
            lw_assert(new.fields.wait_id != LW_WAITER_ID_MAX);
            new.fields.wait_id = LW_WAITER_ID_MAX;
            do_signal = TRUE;
        } else {
            do_signal = FALSE;
        }
    } while (!lw_uint32_swap(&elem->refcnt.atomic, &old.atomic, new.atomic));

    if (do_signal) {
        /*  Elem needs to be destroyed. */
        lw_assert(new.fields.mask);
        lw_assert(old.fields.mask);
        lw_assert(new.fields.pin_count == 0);
        lw_assert(old.fields.pin_count == 1);
        lw_assert(old.fields.wait_id != LW_WAITER_ID_MAX);
        lw_assert(new.fields.wait_id == LW_WAITER_ID_MAX);
        lw_waiter_t *waiter = list->domain->id2waiter(list->domain, old.fields.wait_id);
        lw_waiter_wakeup(waiter, elem);
    }
    lw_assert(((int)lw_atomic32_dec_with_ret(&list->refcnt)) >= 0);
}

/*
 * This is a special routine used internally to give preference to
 * iterators over delete attempts. A element could be masked for delete
 * but the delete operation is waiting for existing pin refs to be taken
 * out. The iterator (that goes BACKWARD) needs to step through this element
 * in order to get to earlier elements. However, it does need to pin the
 * element to ensure it won't get deleted while it is stepping through.
 * This routine allows a pin to go through on masked elements for which
 * the delete hasn't triggered yet. If the delete is already triggered,
 * then the calling iterator defers to the delete and waits for it to
 * finish.
 *
 * @param elem (i) the element that is masked and needs to be pinned.
 *          This element must already be read locked.
 *
 * @return TRUE if the forced pin succeeded. FALSE otherwise.
 *
 */
static lw_bool_t
adl_elem_force_pin_masked(adelem_t *const elem)
{
    adl_refcnt_t old, new;
    /* Confirm that given elem is pinned and locked */
    lw_assert(lw_rwlock_trywrlock(&elem->lock) != 0);

    lw_assert(elem->refcnt.fields.mask);
    old.atomic = elem->refcnt.atomic;
    do {
        new.atomic = old.atomic;
        lw_assert(new.fields.mask);
        if (new.fields.pin_count == 0) {
            /* Delete operation is already underway. Can't pin */
            return FALSE;
        }
        new.fields.pin_count += 1;
    } while (!lw_uint32_swap(&elem->refcnt.atomic, &old.atomic, new.atomic));

    lw_verify(new.fields.pin_count != 0); /* No overflow */
    return TRUE;
}

/**
 * Companion to the above function that is called when a force pin fails. The
 * delete operation in this case is given priority over the iteration. The
 * iterator holds the lock on the element to be deleted which will prevent
 * delete from proceeding. The iterator records itself as waiting using the
 * wait_id field of refcnt and releases the lock on the element. The delete
 * function will wake up the iterator after the delete.
 *
 * @param list (i) the list concerned.
 * @param elem (i/o) the element that is under delete. The caller must have it
 *      read locked. The lock is released in this function.
 * @param waiter (i/o) the waiter to use for the caller.
 *
 * @return none.
 */
static void
adl_elem_delete_wait(adlist_t *const list,
                     adelem_t *const elem,
                     lw_waiter_t *const waiter)
{
    adl_refcnt_t old, new;
    lw_assert(lw_rwlock_trywrlock(&elem->lock) != 0);
    adl_assert_elem_on_list(list, elem);
    old.atomic = elem->refcnt.atomic;
    lw_waiter_set_src(waiter, list);
    waiter->event.tag = LW_PTR_2_NUM(elem, lw_uint64_t);
    do {
        new.atomic = old.atomic;
        lw_assert(old.fields.pin_count == 0);
        lw_assert(old.fields.mask == 1);
        waiter->next = new.fields.wait_id;
        new.fields.wait_id = waiter->id;
    } while (!lw_uint32_swap(&elem->refcnt.atomic, &old.atomic, new.atomic));

    /* Release lock on element so delete can proceed. */
    adl_unlock(list, &elem->lock, ADL_LOCK_SHARED);
    /* Now wait to be signalled. */
    lw_waiter_wait(waiter);
    lw_waiter_clear_src(waiter);
    waiter->event.tag = 0;
}

/**
 * Get current pin count of elem.
 */
static inline lw_uint32_t
adl_elem_get_pincount(adelem_t *const elem)
{
    adl_refcnt_t current;
    current.atomic = elem->refcnt.atomic;
    return current.fields.pin_count;
}

/**
 * Mask an element for delete. Calling thread must have element pinned already.
 */
static inline adl_remove_result_t
adl_elem_mask(adelem_t *const elem)
{
    adl_refcnt_t old, new;

    old.atomic = elem->refcnt.atomic;
    do {
        new.atomic = old.atomic;
        if (old.fields.mask) {
            return ADL_REMOVE_FAIL;
        }
        lw_assert(old.fields.pin_count > 0);
        new.fields.mask = 1;
        if (new.fields.pin_count == 1) {
            /* This is the only thread with the pin. Can do the
             * delete right away.
             */
            new.fields.pin_count = 0;
        }
    } while (!lw_uint32_swap(&elem->refcnt.atomic, &old.atomic, new.atomic));

    lw_assert(old.fields.mask == 0);
    lw_assert(new.fields.mask == 1);
    if (new.fields.pin_count == 0) {
        /* No other thread has elem pinned */
        return ADL_REMOVE_SUCCESS;
    } else {
        /* Need to wait for other threads to unpin */
        return ADL_REMOVE_SUCCESS_WAIT;
    }
}

/**
 * Unmask an element to expose it back to the list. This is not the counter-part to the
 * elem_mask function above. Rather, this is used at the end of an insert operation to
 * make the element visible to other threads accessing the list. The reason for inserting
 * an element in masked state and then unmask it at the end is really to deal with 2 racing
 * deletes on the same element. Thread1 removes and then inserts the element. Thread2 should
 * not be able to do any part of the remove operation on the element until is back into the list.
 * Keeping the element masked till the very end ensures that Thread2 will either get a remove
 * failure or will only get to mark the element after it is back fully on the list.
 */
static inline void
adl_elem_expose(adelem_t *const elem, LW_IN lw_bool_t pinned)
{
    adl_refcnt_t old, new;

    LW_UNUSED_PARAMETER(pinned);
    old.atomic = elem->refcnt.atomic;
    do {
        new.atomic = old.atomic;
        lw_assert(old.fields.mask == 1);
        lw_assert(old.fields.pin_count == 1 || !pinned);
        lw_assert(old.fields.wait_id == LW_WAITER_ID_MAX);
        new.fields.mask = 0;
    } while (!lw_uint32_swap(&elem->refcnt.atomic, &old.atomic, new.atomic));
}

/**
 * Initialize a list, preparing it for use.
 *
 * @param list (i/o) the list to be initialized.
 */
void
adl_init(adlist_t *const list,
         char const *name,
         lw_waiter_domain_t *domain)
{
#ifdef LW_DEBUG
    list->magic = ADL_INITIALIZED;
    lw_atomic32_set(&list->refcnt, 0);
#endif
    list->head = list->tail = NULL;
    if (domain == NULL) {
        domain = lw_waiter_global_domain;
    }
    list->domain = domain;
    list->name = name;
    lw_atomic32_set(&list->count, 0);
    lw_rwlock_init(&list->head_lock, LW_RWLOCK_FAIR);
    lw_rwlock_init(&list->tail_lock, LW_RWLOCK_FAIR);
}

/**
 * Destroy a list, ensuring it cannot be used again.
 *
 * @param list (i/o) the list to be initialized.
 */
void
adl_destroy(adlist_t *const list)
{
#ifdef LW_DEBUG
    list->magic = 0;
    lw_verify(lw_atomic32_read(&list->refcnt) == 0);
#endif
    lw_verify(lw_atomic32_read(&list->count) == 0);
    lw_verify(list->head == NULL);
    lw_verify(list->tail == NULL);
    lw_rwlock_destroy(&list->head_lock);
    lw_rwlock_destroy(&list->tail_lock);
}

/**
 * Determine if the specified list is empty.
 *
 * @param list (i) the list to be counted.
 *
 * @return TRUE if the list is empty, FALSE otherwise.
 */
lw_bool_t
adl_is_empty(const adlist_t *list)
{
    lw_assert(lw_atomic32_inc_with_ret(lw_cast_no_const(&list->refcnt)) > 0);
    lw_bool_t const ret = (lw_atomic32_read(&list->count) == 0);
    lw_assert(((int)lw_atomic32_dec_with_ret(lw_cast_no_const(&list->refcnt))) >= 0);
    return ret;
}

/**
 * Determine the number of elements in the specified list.
 *
 * Note that this interface is very fast in non-debug build
 *
 * @param list (i) the list to count.
 *
 * @return The number of elements in the list.
 */
lw_uint32_t
count(const adlist_t *list)
{
    lw_assert(lw_atomic32_inc_with_ret(lw_cast_no_const(&list->refcnt)) > 0);
    lw_uint32_t const ret = lw_atomic32_read(&list->count);
    lw_assert(((int)lw_atomic32_dec_with_ret(lw_cast_no_const(&list->refcnt))) >= 0);
    return ret;
}

/**
 * Initialize the element for use in an adlist_t;
 *
 * @param elem (i/o) the list element.
 */
static inline void
adl_elem_init(adlist_t *const list,
              adelem_t *const elem,
              LW_IN lw_bool_t pinned)
{
    adl_refcnt_t refcnt;
    elem->next = elem->prev = ADL_DBG_BADELEM;
    lw_rwlock_init(&elem->lock, LW_RWLOCK_FAIR);
    refcnt.fields.mask = 1;
    refcnt.fields.pin_count = (pinned) ? 1 : 0;
    refcnt.fields.wait_id = LW_WAITER_ID_MAX;
    elem->refcnt.atomic = refcnt.atomic;

#ifdef LW_DEBUG
    /*
     * If LW_DEBUG is set, update the list pointer so
     * that it is easily recognizable.
     */
    elem->list = list;
#else
    LW_UNUSED_PARAMETER(list);
#endif
}

/**
 * Destroy the element for use in an adlist_t;
 *
 * @param elem (i/o) the list element.
 */
static inline void
adl_elem_clear(adlist_t *const list, adelem_t *const elem)
{
    int ret;
    LW_UNUSED_PARAMETER(ret); // for non-debug builds.
    lw_assert(elem->list != ADL_DBG_BADLIST);
    /* elem must be locked */
    lw_assert(lw_rwlock_tryrdlock(&elem->lock) != 0);
    /* element must be masked and unpinned */
    lw_assert(elem->refcnt.fields.mask == 1);
    lw_assert(elem->refcnt.fields.pin_count == 0);
    if (elem->refcnt.fields.wait_id != LW_WAITER_ID_MAX) {
        /* Some iterator is waiting that can now be woken up.
         * By this time, no other iterator should be able to add
         * itself to the wait queue as they can at best be waiting
         * on the lock of the element.
         *
         * Note: the use of lw_waiter_global_domain below instead
         * of adlist->domain is deliberate as this is dealing with
         * waiting threads, not domain for elements of the list. By
         * and large this is confusing and it is unclear if it is worth
         * trying to keep a separate domain for an adlist. The uses and
         * wisdom of it is unclear.
         */
        lw_waiter_wake_all(lw_waiter_global_domain,
                           elem->refcnt.fields.wait_id,
                           elem);
        elem->refcnt.fields.wait_id = LW_WAITER_ID_MAX;
    }

    if (lw_rwlock_has_waiters(&elem->lock)) {
        /* Now put the caller on the wait list for the elem lock. This
         * is to ensure that anyone waiting on the element already is
         * done before we trash it.
         */
        /*
         * list could be destroyed already. It is the user's responsibility
         * to avoid this situation.
         */
        ret = adl_lock_async(list, &elem->lock, ADL_LOCK_EXCLUSIVE);
        lw_assert(ret != 0);
        adl_unlock(list, &elem->lock, ADL_LOCK_EXCLUSIVE);
        adl_lock_wait(list, &elem->lock, ADL_LOCK_EXCLUSIVE);
    }
    lw_assert(lw_rwlock_tryrdlock(&elem->lock) != 0);
    lw_verify(!lw_rwlock_has_waiters(&elem->lock));

    /* Clear pointers */

    elem->next = elem->prev = ADL_DBG_BADELEM;
#ifdef LW_DEBUG
    /*
     * If LW_DEBUG is set, update the list pointer.
     */
    elem->list = ADL_DBG_BADLIST;
#endif
    /* Unlock locks */
    adl_unlock(list, &elem->lock, ADL_LOCK_EXCLUSIVE);

    /* Now there should be no more refs for this element
     * and we can destroy it.
     */
    lw_rwlock_destroy(&elem->lock);
    /* The refcnt is left masked and possibly pinned so that any racy attempt
     * to delete it will get a failure.
     */
    lw_assert(elem->refcnt.fields.mask);
    lw_assert(elem->refcnt.fields.pin_count == 0);
}

/*
 * The checks are racy. The caller is responsible for ensuring the
 * element isn't being added to any list during this call.
 */
lw_bool_t
adl_elem_not_on_any_list(adelem_t const *const elem)
{
#ifdef LW_DEBUG
    /* Can simply check if the list ptr is non-null */
    if (elem->list != ADL_DBG_BADLIST) {
        return FALSE;
    }
    lw_assert(elem->prev == ADL_DBG_BADELEM);
    lw_assert(elem->next == ADL_DBG_BADELEM);
    lw_assert(elem->refcnt.fields.mask);
    return TRUE;
#else
    if (elem->prev == ADL_DBG_BADELEM) {
        lw_verify(elem->next == ADL_DBG_BADELEM);
        lw_verify(elem->refcnt.fields.mask);
        return TRUE;
    }
    return FALSE;
#endif
}

/**
 * Get the first element in a list. The returned element is pinned.
 *
 * @param list (i) the list from which the first element is
 *     requested.
 *
 * @return the first element in the list if the list is not empty,
 *    a NULL pointer otherwise.
 */
void *
_adl_first(adlist_t *const list)
{
    adelem_t *first_elem;
    lw_rwlock_t *head_lock = &list->head_lock;

    lw_assert(lw_atomic32_inc_with_ret(&list->refcnt) > 0);
    adl_lock_sync(list, head_lock, ADL_LOCK_SHARED);
    first_elem = ADL_LIST_HEAD(list);

    while (first_elem != NULL &&
           !adl_elem_pin(first_elem)) {
        adl_lock_sync(list, &first_elem->lock, ADL_LOCK_SHARED);
        adl_unlock(list, head_lock, ADL_LOCK_SHARED);
        head_lock = &first_elem->lock;
        first_elem = ADL_ELEM_NEXT(first_elem);
    }

    adl_unlock(list, head_lock, ADL_LOCK_SHARED);
    adl_assert_elem_is_pinned(first_elem);
    adl_assert_elem_on_list(list, first_elem);
    lw_assert(((int)lw_atomic32_dec_with_ret(&list->refcnt)) >= 0);
    return first_elem;
}

/**
 * Get the last element in a list. The returned element is pinned.
 *
 * @param list (i) the list from which the last element is
 *     requested.
 *
 * @return the last element in the list if the list is not empty,
 *    a NULL pointer otherwise.
 */
void *
_adl_last(adlist_t *const list)
{
    int ret;
    adelem_t *last_elem;
    lw_assert(lw_atomic32_inc_with_ret(&list->refcnt) > 0);
    adl_lock_sync(list, &list->tail_lock, ADL_LOCK_SHARED);

    last_elem = ADL_LIST_TAIL(list);

    while (last_elem != NULL &&
           !adl_elem_pin(last_elem)) {
        /* last elem is marked for delete. The delete
         * won't proceed until this thread lets go of
         * the lock it has on tail. Async lock the last_elem
         * before releasing elem.
         */
        ret = adl_lock_async(list, &last_elem->lock, ADL_LOCK_SHARED);
        if (ret != 0) {
            lw_assert(ret == EWOULDBLOCK);
            adl_unlock(list, &list->tail_lock, ADL_LOCK_SHARED);
            adl_lock_wait(list, &last_elem->lock, ADL_LOCK_SHARED);
            adl_lock_sync(list, &list->tail_lock, ADL_LOCK_SHARED);
        }
        if (last_elem != ADL_LIST_TAIL(list)) {
            adl_unlock(list, &last_elem->lock, ADL_LOCK_SHARED);
            last_elem = ADL_LIST_TAIL(list);
            continue;
        }
        /*
         * See note in _adl_prev as to why we need to do it this way.
         */
        if (adl_elem_force_pin_masked(last_elem)) {
            /* Managed to force pin the element. We can safely
             * traverse across it.
             */
            adl_unlock(list, &list->tail_lock, ADL_LOCK_SHARED);
            adl_unlock(list, &last_elem->lock, ADL_LOCK_SHARED);
            last_elem = _adl_prev(list, last_elem);
            adl_assert_elem_is_pinned(last_elem);
            adl_assert_elem_on_list(list, last_elem);
            lw_assert(((int)lw_atomic32_dec_with_ret(&list->refcnt)) >= 0);
            return last_elem;
        } else {
            /* Delete operation has already started. Need to give
             * it priority, so this iterator has to wait someplace.
             * The someplace is the wait_id field of the refcnt.
             */
            lw_waiter_t *waiter = lw_waiter_get();
            lw_assert(waiter->next == LW_WAITER_ID_MAX);
            adl_unlock(list, &list->tail_lock, ADL_LOCK_SHARED); /* So delete can proceed */
            adl_elem_delete_wait(list, last_elem, waiter);
            /* prev element is gone when we return from above. */
            adl_lock_sync(list, &list->tail_lock, ADL_LOCK_SHARED); /* Need this again */
            last_elem = ADL_LIST_TAIL(list);
        }
    }
    adl_unlock(list, &list->tail_lock, ADL_LOCK_SHARED);
    adl_assert_elem_is_pinned(last_elem);
    adl_assert_elem_on_list(list, last_elem);
    lw_assert(((int)lw_atomic32_dec_with_ret(&list->refcnt)) >= 0);
    return last_elem;
}

/**
 * Given an element in a list, get the next element in that list.
 * The pin on the given element is implicitly released before return
 * unless the unpin argument is FALSE.
 *
 * @param list (i) the list from which the next element is
 *     requested.
 * @param elem (i) the element for which the next pointer is
 *     requested.
 * @param unpin (i) bool indicating whether to unpin elem internally
 *      or not.
 * @return the next element in the list.  If the specified element is
 *     the last element in the list, return a NULL pointer.
 */
static void *
adl_next_intern(adlist_t *const list,
                adelem_t *const _elem,
                LW_IN lw_bool_t unpin)
{
    adelem_t *next_elem;
    adelem_t *elem = _elem;

    lw_assert(lw_atomic32_inc_with_ret(&list->refcnt) > 0);
    adl_assert_elem_on_list(list, elem);
    adl_assert_elem_is_pinned_or_masked(elem);

    adl_lock_sync(list, &elem->lock, ADL_LOCK_SHARED);

    next_elem = ADL_ELEM_NEXT(elem);

    while (next_elem != NULL &&
           !adl_elem_pin(next_elem)) {
        /* Cant return this element. Return the one after it. */
        adl_lock_sync(list, &next_elem->lock, ADL_LOCK_SHARED);
        adl_unlock(list, &elem->lock, ADL_LOCK_SHARED);
        elem = next_elem;
        next_elem = ADL_ELEM_NEXT(elem);
    }
    adl_unlock(list, &elem->lock, ADL_LOCK_SHARED);
    if (unpin) {
        adl_elem_unpin(list, _elem);
    }

    adl_assert_elem_is_pinned(next_elem);
    adl_assert_elem_on_list(list, next_elem);
    lw_assert(((int)lw_atomic32_dec_with_ret(&list->refcnt)) >= 0);
    return next_elem;
}

void *
_adl_next(adlist_t *const list,
          adelem_t *const _elem)
{
    return adl_next_intern(list, _elem, TRUE);
}

/**
 * Given an element in a list, get the previous element in that list.
 * The pin on the given element is implicitly released before return
 * unless the unpin argument is FALSE.
 *
 * @param list (i) the list from which the previous element is
 *     requested.
 * @param elem (i) the element for which the prev pointer is
 *     requested.
 * @param unpin (i) bool indicating whether to unpin elem internally
 *      or not.
 *
 * @return the previous element in the list.  If the specified element
 *     is the first element in the list, return a NULL pointer.
 */
static void *
adl_prev_intern(adlist_t *const list,
                adelem_t *const _elem,
                LW_IN lw_bool_t _unpin)
{
    int ret;
    adelem_t *prev_elem;
    adelem_t *elem = _elem;
    lw_bool_t unpin = _unpin;

    lw_assert(lw_atomic32_inc_with_ret(&list->refcnt) > 0);
    adl_assert_elem_on_list(list, elem);
    adl_assert_elem_is_pinned_or_masked(elem);

    adl_lock_sync(list, &elem->lock, ADL_LOCK_SHARED);

    prev_elem = ADL_ELEM_PREV(elem);
    adl_assert_elem_on_list(list, prev_elem);

    while (prev_elem != NULL && !adl_elem_pin(prev_elem)) {
        /* Prev elem is marked for delete. The delete
         * won't proceed until this thread lets go of
         * the lock it has on elem. Async lock the prev_elem
         * before releasing elem.
         */
        if (unpin) {
            adl_assert_elem_is_pinned(elem);
        }
        ret = adl_lock_async(list, &prev_elem->lock, ADL_LOCK_SHARED);
        if (ret != 0) {
            /* Expected behavior. Release elem lock and wait for
             * lock on prev_elem.
             */
            lw_assert(ret == EWOULDBLOCK);
            adl_unlock(list, &elem->lock, ADL_LOCK_SHARED);
            adl_lock_wait(list, &prev_elem->lock, ADL_LOCK_SHARED);
            /* Relock the list to check if it changed */
            adl_lock_sync(list, &elem->lock, ADL_LOCK_SHARED);
        }
        if (prev_elem != ADL_ELEM_PREV(elem)) {
            /* list changed already. retry */
            adl_unlock(list, &prev_elem->lock, ADL_LOCK_SHARED);
            prev_elem = ADL_ELEM_PREV(elem);
            continue;
        }
        /* List hasn't changed yet. The delete operation is still
         * waiting. There are no pleasant choices now. We cannot
         * pin prev_element since it is marked for delete. Going to
         * elements before it (which will require releasing lock on
         * this at some point) runs the risk of accessing already deleted
         * elements. If we force a pin here, we run the risk of starving
         * a delete. If we wait for delete to complete, we could starve
         * this operation. Delete latency can be hidden by making them async,
         * so we choose to try to run this operation to completion.
         */
        if (adl_elem_force_pin_masked(prev_elem)) {
            /* Managed to force pin the element. We can safely
             * traverse across it.
             */
            adl_unlock(list, &elem->lock, ADL_LOCK_SHARED);
            if (unpin) {
                adl_elem_unpin(list, elem);
            }
            unpin = TRUE;
            elem = prev_elem;
            prev_elem = ADL_ELEM_PREV(elem);
        } else {
            /* Delete operation has already started. Need to give
             * it priority, so this iterator has to wait someplace.
             * The someplace is the wait_id field of the refcnt.
             */
            lw_waiter_t *waiter = lw_waiter_get();
            lw_assert(waiter->next == LW_WAITER_ID_MAX);
            adl_unlock(list, &elem->lock, ADL_LOCK_SHARED); /* So delete can proceed */
            adl_elem_delete_wait(list, prev_elem, waiter);
            /* prev element is gone when we return from above. */
            adl_lock_sync(list, &elem->lock, ADL_LOCK_SHARED); /* Need this again */
            prev_elem = ADL_ELEM_PREV(elem);
        }
    }

    adl_assert_elem_on_list(list, elem);
    adl_unlock(list, &elem->lock, ADL_LOCK_SHARED);
    if (unpin) {
        adl_elem_unpin(list, elem);
    }
    adl_assert_elem_is_pinned(prev_elem);
    adl_assert_elem_on_list(list, prev_elem);
    lw_assert(((int)lw_atomic32_dec_with_ret(&list->refcnt)) >= 0);
    return prev_elem;
}

void *
_adl_prev(adlist_t *const list,
          adelem_t *const _elem)
{
    return adl_prev_intern(list, _elem, TRUE);
}

/**
 * Do the actual work of removing an element. The element must be masked
 * before calling this function.
 */
void
adl_remove_elem_do(adlist_t *const list,
                   adelem_t *const elem)
{
    int ret;
    adelem_t *prev, *next;
    lw_rwlock_t *prev_lock;
    lw_rwlock_t *next_lock;

    lw_assert(lw_atomic32_inc_with_ret(&list->refcnt) > 0);
    adl_assert_elem_on_list(list, elem);
    lw_assert(elem->refcnt.fields.mask == 1);
    lw_assert(elem->refcnt.fields.pin_count == 0);

    /* Do the actual delete */
    adl_lock_sync(list, &elem->lock, ADL_LOCK_EXCLUSIVE);

retry:
    prev = ADL_ELEM_PREV(elem);

    if (prev == NULL) {
        /* Lock the list itself */
        prev_lock = &list->head_lock;
    } else {
        prev_lock = &prev->lock;
    }

    ret = adl_lock_async(list, prev_lock, ADL_LOCK_EXCLUSIVE);
    if (ret != 0) {
        adl_unlock(list, &elem->lock, ADL_LOCK_EXCLUSIVE);
        adl_lock_wait(list, prev_lock, ADL_LOCK_EXCLUSIVE);
        adl_lock_sync(list, &elem->lock, ADL_LOCK_EXCLUSIVE);
        if (prev != ADL_ELEM_PREV(elem)) {
            /* List changed around elem. Need to retry */
            adl_unlock(list, prev_lock, ADL_LOCK_EXCLUSIVE);
            goto retry;
        }
    }

    next = ADL_ELEM_NEXT(elem);
    if (next == NULL) {
        next_lock = &list->tail_lock;
    } else {
        next_lock = &next->lock;
    }

    adl_lock_sync(list, next_lock, ADL_LOCK_EXCLUSIVE);
    lw_assert(ADL_ELEM_PREV(elem) == prev &&
              ADL_ELEM_NEXT(elem) == next);

    if (prev == NULL) {
        lw_assert(ADL_LIST_HEAD(list) == elem);
        ADL_LIST_HEAD_SET(list, next);
    } else {
        lw_assert(ADL_ELEM_NEXT(prev) == elem);
        ADL_ELEM_NEXT_SET(prev, next);
    }
    if (next == NULL) {
        lw_assert(ADL_LIST_TAIL(list) == elem);
        ADL_LIST_TAIL_SET(list, prev);
    } else {
        lw_assert(ADL_ELEM_PREV(next) == elem);
        ADL_ELEM_PREV_SET(next, prev);
    }

    adl_unlock(list, prev_lock, ADL_LOCK_EXCLUSIVE);
    adl_unlock(list, next_lock, ADL_LOCK_EXCLUSIVE);
    adl_elem_clear(list, elem);
    lw_assert(((int)lw_atomic32_dec_with_ret(&list->refcnt)) >= 0);
    return;
}

/**
 * Given a list and an element within that list, remove it. This function
 * should not be called on the same element by 2 different threads.
 *
 * @param list (i) the list from which the element is to be removed.
 * @param elem (i/o) the list element to remove.
 */
adl_remove_result_t
adl_remove_elem_start(adlist_t *const list,
                      adelem_t *const elem)
{
    adl_remove_result_t mask_result;

    lw_assert(lw_atomic32_inc_with_ret(&list->refcnt) > 0);
    adl_assert_elem_is_pinned(elem);
    adl_assert_elem_on_list(list, elem);
    mask_result = adl_elem_mask(elem);
    if (mask_result == ADL_REMOVE_FAIL) {
        lw_assert(((int)lw_atomic32_dec_with_ret(&list->refcnt)) >= 0);
        return ADL_REMOVE_FAIL;
    }

    /* Decrement list count before returning. */
    lw_assert(!adl_is_empty(list));
    lw_atomic32_dec(&list->count);
    if (mask_result == ADL_REMOVE_SUCCESS_WAIT) {
        lw_assert(((int)lw_atomic32_dec_with_ret(&list->refcnt)) >= 0);
        return ADL_REMOVE_SUCCESS_WAIT;
    }

    /* Can do the actual delete */
    adl_remove_elem_do(list, elem);
    lw_assert(((int)lw_atomic32_dec_with_ret(&list->refcnt)) >= 0);
    return ADL_REMOVE_SUCCESS;
}

/**
 * Wait for remove to finish on an element removed earlier. The remove
 * attempt must have returned ADL_REMOVE_SUCCESS_WAIT.
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
void
adl_remove_elem_wait(adlist_t *const list,
                     adelem_t *const elem,
                     lw_waiter_t *waiter)
{
    adl_refcnt_t old, new;
    lw_assert(lw_atomic32_inc_with_ret(&list->refcnt) > 0);
    if (waiter == NULL) {
        waiter = list->domain->get_waiter(list->domain);
    } else {
        lw_assert(waiter->domain == list->domain);
    }

    lw_assert(waiter->next == LW_WAITER_ID_MAX);
    old.atomic = elem->refcnt.atomic;
    do {
        lw_assert(old.fields.wait_id == LW_WAITER_ID_MAX);
        lw_assert(old.fields.pin_count >= 1);
        lw_assert(old.fields.mask == 1);
        new.atomic = old.atomic;
        new.fields.pin_count -= 1;
        if (new.fields.pin_count != 0) {
            new.fields.wait_id = waiter->id;
        }
    } while (!lw_uint32_swap(&elem->refcnt.atomic, &old.atomic, new.atomic));

    lw_verify(old.fields.wait_id == LW_WAITER_ID_MAX &&
              old.fields.mask == 1 &&
              old.fields.pin_count >= 1);

    if (new.fields.pin_count != 0) {
        /* Need to wait. */
        lw_waiter_set_src(waiter, list);
        waiter->event.tag = LW_PTR_2_NUM(elem, lw_uint64_t);
        lw_waiter_wait(waiter);
        waiter->event.tag = 0;
        lw_waiter_clear_src(waiter);
    } else {
        /* Element is ready to delete */
        lw_assert(elem->refcnt.fields.mask == 1);
        lw_assert(elem->refcnt.fields.pin_count == 0);
    }
    lw_assert(((int)lw_atomic32_dec_with_ret(&list->refcnt)) >= 0);
}

/**
 * Check if an element marked for removal is ready to be removed.
 * This must be called before calling adl_remove_elem_wait.
 */
lw_bool_t
adl_remove_elem_ready(adlist_t *const list,
                      adelem_t *const elem)
{
    LW_UNUSED_PARAMETER(list);
    lw_assert(lw_atomic32_inc_with_ret(&list->refcnt) > 0);
    adl_refcnt_t old;
    old.atomic = elem->refcnt.atomic;
    lw_assert(old.fields.wait_id == LW_WAITER_ID_MAX);
    lw_assert(old.fields.pin_count >= 1);
    lw_assert(old.fields.mask == 1);
    lw_assert(((int)lw_atomic32_dec_with_ret(&list->refcnt)) >= 0);
    return (old.fields.pin_count == 1);
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
void *
_adl_dequeue(adlist_t *const list)
{
    adelem_t *first_elem;
    adl_remove_result_t res = ADL_REMOVE_SUCCESS;

    lw_assert(lw_atomic32_inc_with_ret(&list->refcnt) > 0);
    first_elem = _adl_first(list);

    while (first_elem != NULL) {
        res = adl_remove_elem_start(list, first_elem);
        switch (res) {
            case ADL_REMOVE_FAIL:
                adl_elem_unpin(list, first_elem);
                first_elem = _adl_first(list);
                break;
            case ADL_REMOVE_SUCCESS:
                lw_assert(((int)lw_atomic32_dec_with_ret(&list->refcnt)) >= 0);
                return first_elem;
            case ADL_REMOVE_SUCCESS_WAIT:
                adl_remove_elem_finish(list, first_elem, NULL);
                lw_assert(((int)lw_atomic32_dec_with_ret(&list->refcnt)) >= 0);
                return first_elem;
            default:
                lw_verify(FALSE);
        }
    }

    lw_assert(first_elem == NULL);
    lw_assert(((int)lw_atomic32_dec_with_ret(&list->refcnt)) >= 0);
    return NULL;
}

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
void *
_adl_pop(adlist_t *const list)
{
    adelem_t *last_elem;
    adl_remove_result_t res = ADL_REMOVE_SUCCESS;

    lw_assert(lw_atomic32_inc_with_ret(&list->refcnt) > 0);
    last_elem = _adl_last(list);

    while (last_elem != NULL) {
        res = adl_remove_elem_start(list, last_elem);
        switch (res) {
            case ADL_REMOVE_FAIL:
                adl_elem_unpin(list, last_elem);
                last_elem = _adl_last(list);
                break;
            case ADL_REMOVE_SUCCESS:
                lw_assert(((int)lw_atomic32_dec_with_ret(&list->refcnt)) >= 0);
                return last_elem;
            case ADL_REMOVE_SUCCESS_WAIT:
                adl_remove_elem_finish(list, last_elem, NULL);
                lw_assert(((int)lw_atomic32_dec_with_ret(&list->refcnt)) >= 0);
                return last_elem;
            default:
                lw_verify(FALSE);
        }
    }

    lw_assert(last_elem == NULL);
    lw_assert(((int)lw_atomic32_dec_with_ret(&list->refcnt)) >= 0);
    return NULL;
}

/**
 * Insert the specified element at the front of the specified list.
 *
 * @param list (i/o) the list to which the element is to be inserted.
 * @param new (i/o) the list element to be inserted.
 */
void
adl_insert_at_front(adlist_t *const list,
                    adelem_t *const new,
                    LW_IN lw_bool_t return_pinned)
{
    lw_assert(lw_atomic32_inc_with_ret(&list->refcnt) > 0);
    adl_elem_init(list, new, return_pinned);
    adl_lock_sync(list, &list->head_lock, ADL_LOCK_EXCLUSIVE);
    if (ADL_LIST_HEAD(list) == NULL) {
        adl_lock_sync(list, &list->tail_lock, ADL_LOCK_EXCLUSIVE);
        lw_assert(ADL_LIST_TAIL(list) == NULL);
        ADL_LIST_TAIL_SET(list, new);
        ADL_LIST_HEAD_SET(list, new);
        new->next = new->prev = NULL;
        lw_atomic32_inc(&list->count); /* Must update before expose */
        adl_elem_expose(new, return_pinned);
        adl_unlock(list, &list->tail_lock, ADL_LOCK_EXCLUSIVE);
    } else {
        adelem_t *first_elem = ADL_LIST_HEAD(list);
        adl_lock_sync(list, &first_elem->lock, ADL_LOCK_EXCLUSIVE);
        lw_assert(ADL_ELEM_PREV(first_elem) == NULL);
        lw_assert(ADL_LIST_HEAD(list) == first_elem);
        ADL_ELEM_PREV_SET(first_elem, new);
        new->prev = NULL;
        new->next = first_elem;
        ADL_LIST_HEAD_SET(list, new);
        lw_atomic32_inc(&list->count); /* Must update before expose */
        adl_elem_expose(new, return_pinned);
        adl_unlock(list, &first_elem->lock, ADL_LOCK_EXCLUSIVE);
    }
    adl_unlock(list, &list->head_lock, ADL_LOCK_EXCLUSIVE);
#ifdef LW_DEBUG
    if (return_pinned) {
        adl_assert_elem_is_pinned(new);
        adl_assert_elem_on_list(list, new);
    }
#endif
    lw_assert(((int)lw_atomic32_dec_with_ret(&list->refcnt)) >= 0);
}

/**
 * Append the specified element at the end of the specified list.
 *
 * @param list (i/o) the list to which the element is to be appended.
 * @param new (i/o) the list element to be inserted.
 */
void
adl_append_at_end(adlist_t *const list,
                  adelem_t *const new,
                  LW_IN lw_bool_t return_pinned)
{
    int ret;
    lw_assert(lw_atomic32_inc_with_ret(&list->refcnt) > 0);
    adl_elem_init(list, new, return_pinned);
    adl_lock_sync(list, &list->tail_lock, ADL_LOCK_EXCLUSIVE);
retry:
    if (ADL_LIST_TAIL(list) == NULL) {
        ret = adl_lock_async(list, &list->head_lock, ADL_LOCK_EXCLUSIVE);
        if (ret != 0) {
            adl_unlock(list, &list->tail_lock, ADL_LOCK_EXCLUSIVE);
            adl_lock_wait(list, &list->head_lock, ADL_LOCK_EXCLUSIVE);
            adl_lock_sync(list, &list->tail_lock, ADL_LOCK_EXCLUSIVE);
            if (ADL_LIST_TAIL(list) != NULL) {
                /* list changed */
                adl_unlock(list, &list->head_lock, ADL_LOCK_EXCLUSIVE);
                goto retry;
            }
        }
        lw_assert(ADL_LIST_HEAD(list) == NULL);
        ADL_LIST_TAIL_SET(list, new);
        ADL_LIST_HEAD_SET(list, new);
        new->next = new->prev = NULL;
        lw_atomic32_inc(&list->count);
        adl_elem_expose(new, return_pinned);
        adl_unlock(list, &list->head_lock, ADL_LOCK_EXCLUSIVE);
    } else {
        adelem_t *last_elem = ADL_LIST_TAIL(list);
        ret = adl_lock_async(list, &last_elem->lock, ADL_LOCK_EXCLUSIVE);
        if (ret != 0) {
            adl_unlock(list, &list->tail_lock, ADL_LOCK_EXCLUSIVE);
            adl_lock_wait(list, &last_elem->lock, ADL_LOCK_EXCLUSIVE);
            adl_lock_sync(list, &list->tail_lock, ADL_LOCK_EXCLUSIVE);
            if (ADL_LIST_TAIL(list) != last_elem) {
                /* List changed */
                adl_unlock(list, &last_elem->lock, ADL_LOCK_EXCLUSIVE);
                goto retry;
            }
        }
        lw_assert(ADL_ELEM_NEXT(last_elem) == NULL);
        ADL_ELEM_NEXT_SET(last_elem, new);
        new->prev = last_elem;
        new->next = NULL;
        ADL_LIST_TAIL_SET(list, new);
        lw_atomic32_inc(&list->count);
        adl_elem_expose(new, return_pinned);
        adl_unlock(list, &last_elem->lock, ADL_LOCK_EXCLUSIVE);
    }
    adl_unlock(list, &list->tail_lock, ADL_LOCK_EXCLUSIVE);
#ifdef LW_DEBUG
    if (return_pinned) {
        adl_assert_elem_is_pinned(new);
        adl_assert_elem_on_list(list, new);
    }
#endif
    lw_assert(((int)lw_atomic32_dec_with_ret(&list->refcnt)) >= 0);
}

/**
 * Insert the specified new element in front of the specified element
 * in the specified list. The elem pointer must be pinned before
 * calling the function. The new element is returned pinned.
 *
 * @param list (i/o) the list into which the element is to be inserted.
 * @param elem (i/o) the element before which the new element will be
 *     inserted. Must be pinned.
 * @param new (i/o) the list element to be inserted. Is pinned on return.
 *
 */
void
adl_insert_elem_before(adlist_t *const list,
                       adelem_t *const elem,
                       adelem_t *const new,
                       LW_IN lw_bool_t return_pinned)
{
    int ret;
    adelem_t *prev;
    lw_rwlock_t *prev_lock;

    lw_assert(lw_atomic32_inc_with_ret(&list->refcnt) > 0);
    adl_assert_elem_is_pinned(elem);
    adl_assert_elem_on_list(list, elem);

    adl_elem_init(list, new, return_pinned);
    adl_lock_sync(list, &elem->lock, ADL_LOCK_EXCLUSIVE);
retry:
    prev = ADL_ELEM_PREV(elem);
    if (prev == NULL) {
        /* elem is first element */
        prev_lock = &list->head_lock;
    } else {
        prev_lock = &prev->lock;
    }

    ret = adl_lock_async(list, prev_lock, ADL_LOCK_EXCLUSIVE);
    if (ret != 0) {
        adl_unlock(list, &elem->lock, ADL_LOCK_EXCLUSIVE);
        adl_lock_wait(list, prev_lock, ADL_LOCK_EXCLUSIVE);
        adl_lock_sync(list, &elem->lock, ADL_LOCK_EXCLUSIVE);
        if (ADL_ELEM_PREV(elem) != prev) {
            /* List changed. elem is also possibly deleted by now.
             * This is where we want caller to ensure the elem is
             * not deleted.
             */
            adl_unlock(list, prev_lock, ADL_LOCK_EXCLUSIVE);
            goto retry;
        }
    }
    new->next = elem;
    new->prev = prev;
    ADL_ELEM_PREV_SET(elem, new);
    if (prev == NULL) {
        ADL_LIST_HEAD_SET(list, new);
    } else {
        ADL_ELEM_NEXT_SET(prev, new);
    }
    lw_atomic32_inc(&list->count);
    adl_elem_expose(new, return_pinned);
    adl_unlock(list, prev_lock, ADL_LOCK_EXCLUSIVE);
    adl_unlock(list, &elem->lock, ADL_LOCK_EXCLUSIVE);
    adl_assert_elem_is_pinned(elem);
    adl_assert_elem_on_list(list, elem);
#ifdef LW_DEBUG
    if (return_pinned) {
        adl_assert_elem_is_pinned(new);
        adl_assert_elem_on_list(list, new);
    }
#endif
    lw_assert(((int)lw_atomic32_dec_with_ret(&list->refcnt)) >= 0);
}

/**
 * Insert the specified new element after the specified element
 * in the specified list. The elem pointer must be pinned and
 * the new element will be pinned on return.
 *
 * @param list (i/o) the list into which the element is to be inserted.
 * @param elem (i/o) the element after which the new element will be
 *     inserted. Must be pinned.
 * @param new (i/o) the list element to be inserted. Is pinned on return.
 *
 */
void
adl_insert_elem_after(adlist_t *const list,
                      adelem_t *const elem,
                      adelem_t *const new,
                      LW_IN lw_bool_t return_pinned)
{
    adelem_t *next;

    lw_assert(lw_atomic32_inc_with_ret(&list->refcnt) > 0);
    adl_assert_elem_on_list(list, elem);
    adl_assert_elem_is_pinned(elem);

    adl_elem_init(list, new, return_pinned);

    adl_lock_sync(list, &elem->lock, ADL_LOCK_EXCLUSIVE);
    next = ADL_ELEM_NEXT(elem);
    new->prev = elem;
    new->next = next;
    ADL_ELEM_NEXT_SET(elem, new);
    if (next == NULL) {
        adl_lock_sync(list, &list->tail_lock, ADL_LOCK_EXCLUSIVE);
        ADL_LIST_TAIL_SET(list, new);
        lw_atomic32_inc(&list->count);
        adl_elem_expose(new, return_pinned);
        adl_unlock(list, &list->tail_lock, ADL_LOCK_EXCLUSIVE);
    } else {
        adl_lock_sync(list, &next->lock, ADL_LOCK_EXCLUSIVE);
        ADL_ELEM_PREV_SET(next, new);
        lw_atomic32_inc(&list->count);
        adl_elem_expose(new, return_pinned);
        adl_unlock(list, &next->lock, ADL_LOCK_EXCLUSIVE);
    }
    adl_unlock(list, &elem->lock, ADL_LOCK_EXCLUSIVE);
    adl_assert_elem_is_pinned(elem);
    adl_assert_elem_on_list(list, elem);
#ifdef LW_DEBUG
    if (return_pinned) {
        adl_assert_elem_is_pinned(new);
        adl_assert_elem_on_list(list, new);
    }
#endif
    lw_assert(((int)lw_atomic32_dec_with_ret(&list->refcnt)) >= 0);
}

#define ITER_DONE(iter) ((iter)->current_elem == ADL_DBG_BADELEM)
#define ITER_SET_DONE(iter) (iter)->current_elem = ADL_DBG_BADELEM

/* Initialize an adlist iter */
void
adlist_iter_init(adlist_iter_t *const iter,
                 adlist_t *const list,
                 LW_IN adl_iter_direction_t direction)
{
    lw_assert(list != NULL);
    iter->list = list;
    iter->direction = direction;
    iter->current_elem = NULL;
    iter->count = 0;
    iter->return_current = FALSE;
}

void
adlist_iter_destroy(adlist_iter_t *const iter)
{
    if (iter->current_elem != NULL &&
        !ITER_DONE(iter)) {
        /* Release lock on current element */
        adl_elem_unpin(iter->list, iter->current_elem);
    }
    iter->list = NULL;
    iter->current_elem = NULL;
}

/**
 * Return next element in iteration. Return NULL if done. The pin on the
 * element returned in previous call to this function will be implicitly
 * released.
 */
void *
_adlist_iter_next(adlist_iter_t *const iter)
{
    if (ITER_DONE(iter)) {
        /* Itertion is done. */
        return NULL;
    }

    if (iter->return_current) {
        iter->return_current = FALSE;
        /* Subtract count so that it remains unchanged
         * upon increment at the bottom of this function.
         */
        iter->count--;
    } else {
        switch (iter->direction) {
            case ADL_ITER_FORWARD:
                if (iter->current_elem == NULL) {
                    iter->current_elem = _adl_first(iter->list);
                } else {
                    iter->current_elem = _adl_next(iter->list,
                                                  iter->current_elem);
                }
                break;
            case ADL_ITER_BACKWARD:
                if (iter->current_elem == NULL) {
                    iter->current_elem = _adl_last(iter->list);
                } else {
                    iter->current_elem = _adl_prev(iter->list,
                                                  iter->current_elem);
                }
                break;
            default:
                lw_verify(FALSE);
        }
    }

    adl_assert_elem_is_pinned(iter->current_elem);
    adl_assert_elem_on_list(iter->list, iter->current_elem);

    if (iter->current_elem == NULL) {
        /* Iteration is over. */
        ITER_SET_DONE(iter);
        return NULL;
    } else {
        iter->count++;
        return iter->current_elem;
    }
}

/**
 * Pop the current element of the iterator off the list. The return value
 * indicates if the pop was successful or not. Upon success, the iterator
 * also advances, internally, to the next element that will be returned on
 * the next invocation of adlist_iter_next. Upon failure, there is no need
 * to internally advance the iterator. The caller doesn't need to worry
 * about this detail.
 *
 * The start function does the masking of the current element.
 */
lw_bool_t
adlist_iter_pop_start(adlist_iter_t *const iter)
{
    adl_remove_result_t remove_result;
    adelem_t *elem = iter->current_elem;
    adlist_t *list = iter->list;
    lw_verify(!(ITER_DONE(iter) || elem == NULL));
    adl_assert_elem_is_pinned(elem);
    adl_assert_elem_on_list(list, elem);
    if (!adl_elem_pin(elem)) {
        /* Elem already picked for delete */
        return FALSE;
    }
    /* Elem is now double pinned. One inside the iter.
     * and by the call to pin above. Hence the call to
     * adl_remove_elem_start below will never return
     * ADL_REMOVE_SUCCESS.
     */
    remove_result = adl_remove_elem_start(list, elem);
    if (remove_result == ADL_REMOVE_FAIL) {
        adl_elem_unpin(list, elem);
        return FALSE;
    }
    lw_verify(remove_result == ADL_REMOVE_SUCCESS_WAIT);
    return TRUE;
}

void
adlist_iter_pop_finish(adlist_iter_t *const iter)
{
    adelem_t *elem = iter->current_elem;
    adlist_t *list = iter->list;
    adelem_t *next_elem = NULL;
    lw_verify(!(ITER_DONE(iter) || elem == NULL));
    adl_assert_elem_is_pinned(elem);
    adl_assert_elem_on_list(list, elem);
    lw_assert(elem->refcnt.fields.pin_count >= 2);
    next_elem = _adlist_iter_next(iter); /* elem unpinned once */
    iter->return_current = TRUE;
    /* Now we have a mask on the elem but it is also possibly pinned by someone
     * else. We also have a pin on next_elem. Trying to wait for the delete
     * go-ahead while holding pin on next_elem is asking for trouble.
     *
     * To avoid deadlock between 2 (or more) threads with each waiting for
     * go ahead to delete an element but holding a pin (no delete flag) on
     * another, we first need to drop the pin on next_elem.
     */
    if (next_elem == NULL) {
        /* No elem to return. We can simply continue with
         * the delete.
         */
        lw_assert(ITER_DONE(iter));
        adl_remove_elem_finish(list, elem, NULL);
        return;
    }
    lw_assert(!ITER_DONE(iter));
    adl_elem_unpin(list, next_elem);
    /* Now wait for go-ahead on elem to delete */
    adl_remove_elem_wait(list, elem, NULL);
    /* Now re-get next_elem */
    if (iter->direction == ADL_ITER_FORWARD) {
        next_elem = adl_next_intern(list, elem, FALSE);
    } else {
        lw_assert(iter->direction == ADL_ITER_BACKWARD);
        next_elem = adl_prev_intern(list, elem, FALSE);
    }
    /* Reset iter's current element. */
    iter->current_elem = next_elem;
    if (next_elem == NULL) {
        ITER_SET_DONE(iter);
        iter->count--; /* Adjust back the count that was incremented
                        * when we got the earlier non-null next_elem.
                        */
    }
    /* Now we can finally delete the elem. */
    adl_remove_elem_do(list, elem);
    return;
}

/**
 * Set the "current" element of the iterator to the given element.
 * adlist_iter_next will return the element after this one.
 */
lw_bool_t
adlist_iter_seek(adlist_iter_t *const iter,
                 adelem_t *const elem,
                 lw_bool_t need_pin)
{
    lw_assert(iter->list != NULL);
    if (need_pin) {
        if (!adl_elem_pin(elem)) {
            return FALSE;
        }
    }
    adl_assert_elem_on_list(iter->list, elem);
    adl_assert_elem_is_pinned_or_masked(elem);
    if (iter->current_elem != NULL &&
        !ITER_DONE(iter)) {
        /* Need to unpin current elem. */
        adl_elem_unpin(iter->list, iter->current_elem);
    }
    iter->current_elem = elem;
    return TRUE;
}
