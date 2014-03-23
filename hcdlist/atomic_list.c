/*
 * Copyright(c) 2010- Data Domain, Inc.  All rights reserved.
 *
 * DATA DOMAIN CONFIDENTIAL -- This is an unpublished work of Data Domain, Inc.,
 * and is fully protected under copyright and trade secret laws.  You may not view,
 * use, disclose, copy, or distribute this file or any information herein except
 * pursuant to a valid written license from Data Domain.
 */

#include "common.h"
#include "atomic_list.h"

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

#ifdef DD_DEBUG
#define ADL_ELEM_NEXT(elem)     (((elem)->adelem_lock.locked) ? (elem)->adelem_next : ADL_DBG_BADELEM + 5)
#define ADL_ELEM_PREV(elem)     (((elem)->adelem_lock.locked) ? (elem)->adelem_prev : ADL_DBG_BADELEM + 5)
#define ADL_LIST_HEAD(list)     (((list)->adl_head_lock.locked) ? (list)->adl_head : ADL_DBG_BADELEM + 5)
#define ADL_LIST_TAIL(list)     (((list)->adl_tail_lock.locked) ? (list)->adl_tail : ADL_DBG_BADELEM + 5)
#else
#define ADL_ELEM_NEXT(elem)     ((elem)->adelem_next)
#define ADL_ELEM_PREV(elem)     ((elem)->adelem_prev)
#define ADL_LIST_HEAD(list)     ((list)->adl_head)
#define ADL_LIST_TAIL(list)     ((list)->adl_tail)
#endif
#define ADL_ELEM_NEXT_SET(elem, val) \
        do { \
            dd_assert((elem)->adelem_lock.wlocked != 0); \
            (elem)->adelem_next = val; \
        } while (0)
#define ADL_ELEM_PREV_SET(elem, val) \
        do { \
            dd_assert((elem)->adelem_lock.wlocked != 0); \
            (elem)->adelem_prev = val; \
        } while (0)
#define ADL_LIST_HEAD_SET(list, val) \
        do { \
            dd_assert((list)->adl_head_lock.wlocked != 0); \
            (list)->adl_head = val; \
        } while (0)
#define ADL_LIST_TAIL_SET(list, val) \
        do { \
            dd_assert((list)->adl_tail_lock.wlocked != 0); \
            (list)->adl_tail = val; \
        } while (0)

typedef enum {
    ADL_LOCK_SHARED = DD_LWLOCK_SHARED,
    ADL_LOCK_SYNC = DD_LWLOCK_WAIT_INLINE,
    ADL_LOCK_EXCLUSIVE = DD_LWLOCK_EXCLUSIVE,
    ADL_LOCK_ASYNC = DD_LWLOCK_WAIT_DEFERRED,
} adl_lock_attempt_t;

static int
adl_lock(adlist_t *const list,
         dd_lwlock_t *const lock,
         IN adl_lock_attempt_t try_type)
{
    int ret;
    dd_thread_wait_t *waiter = NULL;

    dd_assert(ADL_LOCK_SHARED == DD_LWLOCK_SHARED);
    dd_assert(ADL_LOCK_EXCLUSIVE == DD_LWLOCK_EXCLUSIVE);
    dd_assert(ADL_LOCK_SYNC == DD_LWLOCK_WAIT_INLINE);
    dd_assert(ADL_LOCK_ASYNC == DD_LWLOCK_WAIT_DEFERRED);

    if ((try_type & ADL_LOCK_ASYNC) == ADL_LOCK_ASYNC) {
        waiter = dd_thread_wait_get();
    }

    ret = dd_lwlock_lock(lock, try_type | DD_LWLOCK_WAIT, waiter, &list->adl_stats);
    dd_assert(ret == 0 || ret == EWOULDBLOCK);
    return ret;
}

/**
 * Unlock an element previous returned as locked by one of the calls
 * above.
 */
static void
adl_unlock(adlist_t *const list,
           dd_lwlock_t *const lock,
           IN adl_lock_attempt_t try_type)
{
    dd_lwlock_unlock(lock,
                     (try_type & ADL_LOCK_EXCLUSIVE) == ADL_LOCK_EXCLUSIVE,
                     &list->adl_stats);
}

static inline void
adl_lock_sync(adlist_t *const list,
              dd_lwlock_t *const lock,
              IN adl_lock_attempt_t try_type)
{
    int ret;
    dd_assert(!(try_type & ADL_LOCK_ASYNC));
    ret = adl_lock(list, lock, try_type | ADL_LOCK_SYNC);
    dd_assert(ret == 0);
}

static inline int
adl_lock_async(adlist_t *const list,
               dd_lwlock_t *const lock,
               IN adl_lock_attempt_t try_type)
{
    return adl_lock(list, lock, try_type | ADL_LOCK_ASYNC);
}

static void
adl_lock_wait(adlist_t *const list,
              dd_lwlock_t *const lock,
              IN adl_lock_attempt_t try_type)
{
    
    dd_lwlock_contention_wait(lock,
                              try_type,
                              dd_thread_wait_get(),
                              &list->adl_stats);
}

/**
 * Pin an element so it does not get deleted.
 */
dd_bool_t
adl_elem_pin(adelem_t *const elem)
{
    adl_refcnt_t old, new;
    
    dd_assert(elem != NULL);
    old.aref_atomic = elem->adelem_refcnt.aref_atomic;
    do {
        new.aref_atomic = old.aref_atomic;
        if (old.fields.aref_mask) {
            return FALSE;
        }
        new.fields.aref_pin_count += 1;
    } while (!dd_uint32_swap(&elem->adelem_refcnt.aref_atomic, &old.aref_atomic, new.aref_atomic));
        
    dd_verify(new.fields.aref_pin_count != 0); /* no overflow */
    return TRUE;
}

/**
 * Unpin an element previous returned as pinned by one of the calls
 * above.
 */
void
adl_elem_unpin(adlist_t *const list,
               adelem_t *const elem)
{
    adl_refcnt_t old, new;
    dd_bool_t do_signal = FALSE;

    dd_assert(list != NULL && elem != NULL);
    dd_assert(dd_atomic32_inc_with_ret(&list->adl_refcnt) > 0);
    adl_assert_elem_on_list(list, elem);
    old.aref_atomic = elem->adelem_refcnt.aref_atomic;
    do {
        new.aref_atomic = old.aref_atomic;
        dd_assert(old.fields.aref_pin_count > 0);
        new.fields.aref_pin_count -= 1;
        if (old.fields.aref_mask == 1 &&
            new.fields.aref_pin_count == 0) {
            /* A delete on this element is requested and is now
             * ready to proceed.
             */
            dd_assert(new.fields.aref_wait_id != DD_THREAD_WAIT_ID_MAX);
            new.fields.aref_wait_id = DD_THREAD_WAIT_ID_MAX;
            do_signal = TRUE;
        } else {
            do_signal = FALSE;
        }
    } while (!dd_uint32_swap(&elem->adelem_refcnt.aref_atomic, &old.aref_atomic, new.aref_atomic));

    if (do_signal) {
        /*  Elem needs to be destroyed. */
        dd_assert(new.fields.aref_mask);
        dd_assert(old.fields.aref_mask);
        dd_assert(new.fields.aref_pin_count == 0);
        dd_assert(old.fields.aref_pin_count == 1);
        dd_assert(old.fields.aref_wait_id != DD_THREAD_WAIT_ID_MAX);
        dd_assert(new.fields.aref_wait_id == DD_THREAD_WAIT_ID_MAX);
        dd_thread_wait_t *waiter = list->adl_domain->id2waiter(list->adl_domain,
                                                           old.fields.aref_wait_id);
        dd_thread_wakeup_from_wait(waiter, elem);
    }
    dd_assert(((int)dd_atomic32_dec_with_ret(&list->adl_refcnt)) >= 0);
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
static dd_bool_t
adl_elem_force_pin_masked(adelem_t *const elem)
{
    adl_refcnt_t old, new;
    /* Confirm that given elem is pinned and locked */
    dd_assert(dd_lwlock_trywrlock(&elem->adelem_lock) != 0);
    
    dd_assert(elem->adelem_refcnt.fields.aref_mask);
    old.aref_atomic = elem->adelem_refcnt.aref_atomic;
    do {
        new.aref_atomic = old.aref_atomic;
        dd_assert(new.fields.aref_mask);
        if (new.fields.aref_pin_count == 0) {
            /* Delete operation is already underway. Can't pin */
            return FALSE;
        }
        new.fields.aref_pin_count += 1;
    } while (!dd_uint32_swap(&elem->adelem_refcnt.aref_atomic, &old.aref_atomic, new.aref_atomic));

    dd_verify(new.fields.aref_pin_count != 0); /* No overflow */
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
                     dd_thread_wait_t *const waiter)
{
    adl_refcnt_t old, new;
    TSC(delete_wait); /* defines tsc_delete_wait */
    dd_assert(dd_lwlock_trywrlock(&elem->adelem_lock) != 0);
    adl_assert_elem_on_list(list, elem);
    old.aref_atomic = elem->adelem_refcnt.aref_atomic;
    do {
        new.aref_atomic = old.aref_atomic;
        dd_assert(old.fields.aref_pin_count == 0);
        dd_assert(old.fields.aref_mask == 1);
        waiter->next = new.fields.aref_wait_id;
        new.fields.aref_wait_id = waiter->id;
    } while (!dd_uint32_swap(&elem->adelem_refcnt.aref_atomic, &old.aref_atomic, new.aref_atomic));

    /* Release lock on element so delete can proceed. */
    adl_unlock(list, &elem->adelem_lock, ADL_LOCK_SHARED);
    /* Now wait to be signalled. */
    waiter->event.base.wait_src = list;
    waiter->event.base.tag = DD_PTR_2_NUM(elem, dd_uint64_t);
    dd_perf_mutex_in(&dd_perf_ctx,
                     DD_PERF_MUTEX_LOCK,
                     list->adl_stats.trace_id,
                     &list->adl_stats.trace_gen,
                     list->adl_name,
                     0);
    dd_thread_do_wait(waiter);
    dd_perf_out2(&dd_perf_ctx, DD_PERF_MUTEX_LOCK, (int)waiter->id, 1);
    waiter->event.base.tag = 0;
    dd_atomic32_inc(&list->adl_stats.lock_contentions);
    dd_atomic64_add(&list->adl_stats.lock_contention_cyc, TSC_DIFF_NOW(tsc_delete_wait));
}

/**
 * Get current pin count of elem.
 */
static inline dd_uint32_t
adl_elem_get_pincount(adelem_t *const elem)
{
    adl_refcnt_t current;
    current.aref_atomic = elem->adelem_refcnt.aref_atomic;
    return current.fields.aref_pin_count;
}
    
/**
 * Mask an element for delete. Calling thread must have element pinned already.
 */
static inline adl_remove_result_t
adl_elem_mask(adelem_t *const elem)
{
    adl_refcnt_t old, new;

    old.aref_atomic = elem->adelem_refcnt.aref_atomic;
    do {
        new.aref_atomic = old.aref_atomic;
        if (old.fields.aref_mask) {
            return ADL_REMOVE_FAIL;
        }
        dd_assert(old.fields.aref_pin_count > 0);
        new.fields.aref_mask = 1;
        if (new.fields.aref_pin_count == 1) {
            /* This is the only thread with the pin. Can do the
             * delete right away.
             */
            new.fields.aref_pin_count = 0;
        }
    } while (!dd_uint32_swap(&elem->adelem_refcnt.aref_atomic, &old.aref_atomic, new.aref_atomic));
        
    dd_assert(old.fields.aref_mask == 0);
    dd_assert(new.fields.aref_mask == 1);
    if (new.fields.aref_pin_count == 0) {
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
adl_elem_expose(adelem_t *const elem, IN dd_bool_t pinned)
{
    adl_refcnt_t old, new;

    UNUSED_PARAMETER(pinned);
    old.aref_atomic = elem->adelem_refcnt.aref_atomic;
    do {
        new.aref_atomic = old.aref_atomic;
        dd_assert(old.fields.aref_mask == 1);
        dd_assert(old.fields.aref_pin_count == 1 || !pinned);
        dd_assert(old.fields.aref_wait_id == DD_THREAD_WAIT_ID_MAX);
        new.fields.aref_mask = 0;
    } while (!dd_uint32_swap(&elem->adelem_refcnt.aref_atomic, &old.aref_atomic, new.aref_atomic));
}

/**
 * Initialize a list, preparing it for use.
 *
 * @param list (i/o) the list to be initialized.
 */
void 
adl_init(adlist_t *const list,
         char const *name,
         dd_thread_wait_domain_t *domain)
{
#ifdef DD_DEBUG
    list->adl_magic = ADL_INITIALIZED;
    dd_atomic32_set(&list->adl_refcnt, 0);
#endif
    list->adl_head = list->adl_tail = NULL;
    if (domain == NULL) {
        domain = dd_thread_wait_domain_global;
    }
    list->adl_domain = domain;
    list->adl_name = name;
    dd_atomic32_set(&list->adl_count, 0);
    dd_lwlock_init(&list->adl_head_lock, DD_LWLOCK_FAIR);
    dd_lwlock_init(&list->adl_tail_lock, DD_LWLOCK_FAIR);
    dd_lwlock_stats_init(&list->adl_stats, name);
}

/**
 * Destroy a list, ensuring it cannot be used again.
 *
 * @param list (i/o) the list to be initialized.
 */
void 
adl_destroy(adlist_t *const list)
{
#ifdef DD_DEBUG
    list->adl_magic = 0;
    dd_verify(dd_atomic32_read(&list->adl_refcnt) == 0);
#endif
    dd_verify(dd_atomic32_read(&list->adl_count) == 0);
    dd_verify(list->adl_head == NULL);
    dd_verify(list->adl_tail == NULL);
    dd_lwlock_destroy(&list->adl_head_lock);
    dd_lwlock_destroy(&list->adl_tail_lock);
}

/**
 * Determine if the specified list is empty.
 *
 * @param list (i) the list to be counted.
 *
 * @return TRUE if the list is empty, FALSE otherwise.
 */
dd_bool_t 
adl_is_empty(const adlist_t *list)
{
    dd_assert(dd_atomic32_inc_with_ret(dd_cast_no_const(&list->adl_refcnt)) > 0);
    dd_bool_t const ret = (dd_atomic32_read(&list->adl_count) == 0);
    dd_assert(((int)dd_atomic32_dec_with_ret(dd_cast_no_const(&list->adl_refcnt))) >= 0);
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
dd_uint32_t
adl_count(const adlist_t *list)
{
    dd_assert(dd_atomic32_inc_with_ret(dd_cast_no_const(&list->adl_refcnt)) > 0);
    dd_uint32_t const ret = dd_atomic32_read(&list->adl_count);
    dd_assert(((int)dd_atomic32_dec_with_ret(dd_cast_no_const(&list->adl_refcnt))) >= 0);
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
              IN dd_bool_t pinned)
{
    adl_refcnt_t refcnt;
    elem->adelem_next = elem->adelem_prev = ADL_DBG_BADELEM;
    dd_lwlock_init(&elem->adelem_lock, DD_LWLOCK_FAIR);
    refcnt.fields.aref_mask = 1;
    refcnt.fields.aref_pin_count = (pinned) ? 1 : 0;
    refcnt.fields.aref_wait_id = DD_THREAD_WAIT_ID_MAX;
    elem->adelem_refcnt.aref_atomic = refcnt.aref_atomic;

#ifdef DD_DEBUG
    /*
     * If DD_DEBUG is set, update the list pointer so 
     * that it is easily recognizable.
     */
    elem->adelem_list = list;
#else
    UNUSED_PARAMETER(list);
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
    dd_assert(elem->adelem_list != ADL_DBG_BADLIST);
    /* elem must be locked */
    dd_assert(dd_lwlock_tryrdlock(&elem->adelem_lock) != 0);
    /* element must be masked and unpinned */
    dd_assert(elem->adelem_refcnt.fields.aref_mask == 1);
    dd_assert(elem->adelem_refcnt.fields.aref_pin_count == 0);
    if (elem->adelem_refcnt.fields.aref_wait_id != DD_THREAD_WAIT_ID_MAX) {
        /* Some iterator is waiting that can now be woken up.
         * By this time, no other iterator should be able to add
         * itself to the wait queue as they can at best be waiting
         * on the lock of the element.
         */
        dd_thread_wake_all(dd_thread_wait_domain_global,
                           elem->adelem_refcnt.fields.aref_wait_id,
                           elem);
        elem->adelem_refcnt.fields.aref_wait_id = DD_THREAD_WAIT_ID_MAX;
    }

    if (dd_lwlock_has_waiters(&elem->adelem_lock)) {
        /* Now put the caller on the wait list for the elem lock. This
         * is to ensure that anyone waiting on the element already is
         * done before we trash it.
         */
        /* 
         * list could be destroyed already. It is the user's responsibility
         * to avoid this situation.
         */
        ret = adl_lock_async(list, &elem->adelem_lock, ADL_LOCK_EXCLUSIVE);
        dd_assert(ret != 0);
        adl_unlock(list, &elem->adelem_lock, ADL_LOCK_EXCLUSIVE);
        adl_lock_wait(list, &elem->adelem_lock, ADL_LOCK_EXCLUSIVE);
    }
    dd_assert(dd_lwlock_tryrdlock(&elem->adelem_lock) != 0);
    dd_verify(!dd_lwlock_has_waiters(&elem->adelem_lock));
    
    /* Clear pointers */

    elem->adelem_next = elem->adelem_prev = ADL_DBG_BADELEM;
#ifdef DD_DEBUG
    /*
     * If DD_DEBUG is set, update the list pointer.
     */
    elem->adelem_list = ADL_DBG_BADLIST;
#endif
    /* Unlock locks */
    adl_unlock(list, &elem->adelem_lock, ADL_LOCK_EXCLUSIVE);

    /* Now there should be no more refs for this element
     * and we can destroy it.
     */
    dd_lwlock_destroy(&elem->adelem_lock);
    /* The refcnt is left masked and possibly pinned so that any racy attempt
     * to delete it will get a failure.
     */
    dd_assert(elem->adelem_refcnt.fields.aref_mask);
    dd_assert(elem->adelem_refcnt.fields.aref_pin_count == 0);
}

/* 
 * The checks are racy. The caller is responsible for ensuring the
 * element isn't being added to any list during this call.
 */
dd_bool_t
adl_elem_not_on_any_list(adelem_t const *const elem)
{
#ifdef DD_DEBUG
    /* Can simply check if the list ptr is non-null */
    if (elem->adelem_list != ADL_DBG_BADLIST) {
        return FALSE;
    } 
    dd_assert(elem->adelem_prev == ADL_DBG_BADELEM);
    dd_assert(elem->adelem_next == ADL_DBG_BADELEM);
    dd_assert(elem->adelem_refcnt.fields.aref_mask);
    return TRUE;
#else
    if (elem->adelem_prev == ADL_DBG_BADELEM) {
        dd_verify(elem->adelem_next == ADL_DBG_BADELEM);
        dd_verify(elem->adelem_refcnt.fields.aref_mask);
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
    dd_lwlock_t *head_lock = &list->adl_head_lock;

    dd_assert(dd_atomic32_inc_with_ret(&list->adl_refcnt) > 0);
    adl_lock_sync(list, head_lock, ADL_LOCK_SHARED);
    first_elem = ADL_LIST_HEAD(list);

    while (first_elem != NULL &&
           !adl_elem_pin(first_elem)) {
        adl_lock_sync(list, &first_elem->adelem_lock, ADL_LOCK_SHARED);
        adl_unlock(list, head_lock, ADL_LOCK_SHARED);
        head_lock = &first_elem->adelem_lock;
        first_elem = ADL_ELEM_NEXT(first_elem);
    }
        
    adl_unlock(list, head_lock, ADL_LOCK_SHARED);
    adl_assert_elem_is_pinned(first_elem);
    adl_assert_elem_on_list(list, first_elem);
    dd_assert(((int)dd_atomic32_dec_with_ret(&list->adl_refcnt)) >= 0);
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
    dd_assert(dd_atomic32_inc_with_ret(&list->adl_refcnt) > 0);
    adl_lock_sync(list, &list->adl_tail_lock, ADL_LOCK_SHARED);

    last_elem = ADL_LIST_TAIL(list);

    while (last_elem != NULL &&
           !adl_elem_pin(last_elem)) {
        /* last elem is marked for delete. The delete
         * won't proceed until this thread lets go of
         * the lock it has on tail. Async lock the last_elem
         * before releasing elem.
         */
        ret = adl_lock_async(list, &last_elem->adelem_lock, ADL_LOCK_SHARED);
        if (ret != 0) {
            dd_assert(ret == EWOULDBLOCK);
            adl_unlock(list, &list->adl_tail_lock, ADL_LOCK_SHARED);
            adl_lock_wait(list, &last_elem->adelem_lock, ADL_LOCK_SHARED);
            adl_lock_sync(list, &list->adl_tail_lock, ADL_LOCK_SHARED);
        }
        if (last_elem != ADL_LIST_TAIL(list)) {
            adl_unlock(list, &last_elem->adelem_lock, ADL_LOCK_SHARED);
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
            adl_unlock(list, &list->adl_tail_lock, ADL_LOCK_SHARED);
            adl_unlock(list, &last_elem->adelem_lock, ADL_LOCK_SHARED);
            last_elem = _adl_prev(list, last_elem);
            adl_assert_elem_is_pinned(last_elem);
            adl_assert_elem_on_list(list, last_elem);
            dd_assert(((int)dd_atomic32_dec_with_ret(&list->adl_refcnt)) >= 0);
            return last_elem;
        } else {
            /* Delete operation has already started. Need to give
             * it priority, so this iterator has to wait someplace.
             * The someplace is the wait_id field of the refcnt.
             */
            dd_thread_wait_t *waiter = dd_thread_wait_get();
            dd_assert(waiter->next == DD_THREAD_WAIT_ID_MAX);
            adl_unlock(list, &list->adl_tail_lock, ADL_LOCK_SHARED); /* So delete can proceed */
            adl_elem_delete_wait(list, last_elem, waiter);
            /* prev element is gone when we return from above. */
            adl_lock_sync(list, &list->adl_tail_lock, ADL_LOCK_SHARED); /* Need this again */
            last_elem = ADL_LIST_TAIL(list);
        }
    }
    adl_unlock(list, &list->adl_tail_lock, ADL_LOCK_SHARED);
    adl_assert_elem_is_pinned(last_elem);
    adl_assert_elem_on_list(list, last_elem);
    dd_assert(((int)dd_atomic32_dec_with_ret(&list->adl_refcnt)) >= 0);
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
                IN dd_bool_t unpin)
{
    adelem_t *next_elem;
    adelem_t *elem = _elem;

    dd_assert(dd_atomic32_inc_with_ret(&list->adl_refcnt) > 0);
    adl_assert_elem_on_list(list, elem);
    adl_assert_elem_is_pinned_or_masked(elem);

    adl_lock_sync(list, &elem->adelem_lock, ADL_LOCK_SHARED);

    next_elem = ADL_ELEM_NEXT(elem);

    while (next_elem != NULL &&
           !adl_elem_pin(next_elem)) {
        /* Cant return this element. Return the one after it. */
        adl_lock_sync(list, &next_elem->adelem_lock, ADL_LOCK_SHARED);
        adl_unlock(list, &elem->adelem_lock, ADL_LOCK_SHARED);
        elem = next_elem;
        next_elem = ADL_ELEM_NEXT(elem);
    } 
    adl_unlock(list, &elem->adelem_lock, ADL_LOCK_SHARED);
    if (unpin) {
        adl_elem_unpin(list, _elem);
    }

    adl_assert_elem_is_pinned(next_elem);
    adl_assert_elem_on_list(list, next_elem);
    dd_assert(((int)dd_atomic32_dec_with_ret(&list->adl_refcnt)) >= 0);
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
                IN dd_bool_t _unpin)
{
    int ret;
    adelem_t *prev_elem;
    adelem_t *elem = _elem;
    dd_bool_t unpin = _unpin;

    dd_assert(dd_atomic32_inc_with_ret(&list->adl_refcnt) > 0);
    adl_assert_elem_on_list(list, elem);
    adl_assert_elem_is_pinned_or_masked(elem);

    adl_lock_sync(list, &elem->adelem_lock, ADL_LOCK_SHARED);

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
        ret = adl_lock_async(list, &prev_elem->adelem_lock, ADL_LOCK_SHARED);
        if (ret != 0) {
            /* Expected behavior. Release elem lock and wait for
             * lock on prev_elem.
             */
            dd_assert(ret == EWOULDBLOCK);
            adl_unlock(list, &elem->adelem_lock, ADL_LOCK_SHARED);
            adl_lock_wait(list, &prev_elem->adelem_lock, ADL_LOCK_SHARED);
            /* Relock the list to check if it changed */
            adl_lock_sync(list, &elem->adelem_lock, ADL_LOCK_SHARED);
        }
        if (prev_elem != ADL_ELEM_PREV(elem)) {
            /* list changed already. retry */
            adl_unlock(list, &prev_elem->adelem_lock, ADL_LOCK_SHARED);
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
            adl_unlock(list, &elem->adelem_lock, ADL_LOCK_SHARED);
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
            dd_thread_wait_t *waiter = dd_thread_wait_get();
            dd_assert(waiter->next == DD_THREAD_WAIT_ID_MAX);
            adl_unlock(list, &elem->adelem_lock, ADL_LOCK_SHARED); /* So delete can proceed */
            adl_elem_delete_wait(list, prev_elem, waiter);
            /* prev element is gone when we return from above. */
            adl_lock_sync(list, &elem->adelem_lock, ADL_LOCK_SHARED); /* Need this again */
            prev_elem = ADL_ELEM_PREV(elem);
        }
    }

    adl_assert_elem_on_list(list, elem);
    adl_unlock(list, &elem->adelem_lock, ADL_LOCK_SHARED);
    if (unpin) {
        adl_elem_unpin(list, elem);
    }
    adl_assert_elem_is_pinned(prev_elem);
    adl_assert_elem_on_list(list, prev_elem);
    dd_assert(((int)dd_atomic32_dec_with_ret(&list->adl_refcnt)) >= 0);
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
    dd_lwlock_t *prev_lock;
    dd_lwlock_t *next_lock;
    
    dd_assert(dd_atomic32_inc_with_ret(&list->adl_refcnt) > 0);
    adl_assert_elem_on_list(list, elem);
    dd_assert(elem->adelem_refcnt.fields.aref_mask == 1);
    dd_assert(elem->adelem_refcnt.fields.aref_pin_count == 0);
    
    /* Do the actual delete */
    adl_lock_sync(list, &elem->adelem_lock, ADL_LOCK_EXCLUSIVE);

retry:
    prev = ADL_ELEM_PREV(elem);

    if (prev == NULL) {
        /* Lock the list itself */
        prev_lock = &list->adl_head_lock;
    } else {
        prev_lock = &prev->adelem_lock;
    }

    ret = adl_lock_async(list, prev_lock, ADL_LOCK_EXCLUSIVE);
    if (ret != 0) {
        adl_unlock(list, &elem->adelem_lock, ADL_LOCK_EXCLUSIVE);
        adl_lock_wait(list, prev_lock, ADL_LOCK_EXCLUSIVE);
        adl_lock_sync(list, &elem->adelem_lock, ADL_LOCK_EXCLUSIVE);
        if (prev != ADL_ELEM_PREV(elem)) {
            /* List changed around elem. Need to retry */
            adl_unlock(list, prev_lock, ADL_LOCK_EXCLUSIVE);
            goto retry;
        }
    }
    
    next = ADL_ELEM_NEXT(elem);
    if (next == NULL) {
        next_lock = &list->adl_tail_lock;
    } else {
        next_lock = &next->adelem_lock;
    }

    adl_lock_sync(list, next_lock, ADL_LOCK_EXCLUSIVE);
    dd_assert(ADL_ELEM_PREV(elem) == prev && 
              ADL_ELEM_NEXT(elem) == next);

    if (prev == NULL) {
        dd_assert(ADL_LIST_HEAD(list) == elem);
        ADL_LIST_HEAD_SET(list, next);
    } else {
        dd_assert(ADL_ELEM_NEXT(prev) == elem);
        ADL_ELEM_NEXT_SET(prev, next);
    }
    if (next == NULL) {
        dd_assert(ADL_LIST_TAIL(list) == elem);
        ADL_LIST_TAIL_SET(list, prev);
    } else {
        dd_assert(ADL_ELEM_PREV(next) == elem);
        ADL_ELEM_PREV_SET(next, prev);
    }

    adl_unlock(list, prev_lock, ADL_LOCK_EXCLUSIVE);
    adl_unlock(list, next_lock, ADL_LOCK_EXCLUSIVE);
    adl_elem_clear(list, elem);
    dd_assert(((int)dd_atomic32_dec_with_ret(&list->adl_refcnt)) >= 0);
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
    
    dd_assert(dd_atomic32_inc_with_ret(&list->adl_refcnt) > 0);
    adl_assert_elem_is_pinned(elem);
    adl_assert_elem_on_list(list, elem);
    mask_result = adl_elem_mask(elem);
    if (mask_result == ADL_REMOVE_FAIL) {
        dd_assert(((int)dd_atomic32_dec_with_ret(&list->adl_refcnt)) >= 0);
        return ADL_REMOVE_FAIL;
    }
    
    /* Decrement list count before returning. */
    dd_assert(!adl_is_empty(list));
    dd_atomic32_dec(&list->adl_count);
    if (mask_result == ADL_REMOVE_SUCCESS_WAIT) {
        dd_assert(((int)dd_atomic32_dec_with_ret(&list->adl_refcnt)) >= 0);
        return ADL_REMOVE_SUCCESS_WAIT;
    }

    /* Can do the actual delete */
    adl_remove_elem_do(list, elem);
    dd_assert(((int)dd_atomic32_dec_with_ret(&list->adl_refcnt)) >= 0);
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
                     dd_thread_wait_t *waiter)
{
    adl_refcnt_t old, new;
    dd_assert(dd_atomic32_inc_with_ret(&list->adl_refcnt) > 0);
    if (waiter == NULL) {
        waiter = list->adl_domain->get_waiter(list->adl_domain);
    } else {
        dd_assert(waiter->domain == list->adl_domain);
    }

    dd_assert(waiter->next == DD_THREAD_WAIT_ID_MAX);
    old.aref_atomic = elem->adelem_refcnt.aref_atomic;
    do {
        dd_assert(old.fields.aref_wait_id == DD_THREAD_WAIT_ID_MAX);
        dd_assert(old.fields.aref_pin_count >= 1);
        dd_assert(old.fields.aref_mask == 1);
        new.aref_atomic = old.aref_atomic;
        new.fields.aref_pin_count -= 1;
        if (new.fields.aref_pin_count != 0) {
            new.fields.aref_wait_id = waiter->id;
        }
    } while (!dd_uint32_swap(&elem->adelem_refcnt.aref_atomic, &old.aref_atomic, new.aref_atomic));

    dd_verify(old.fields.aref_wait_id == DD_THREAD_WAIT_ID_MAX &&
              old.fields.aref_mask == 1 &&
              old.fields.aref_pin_count >= 1);

    if (new.fields.aref_pin_count != 0) {
        /* Need to wait. */
        dd_uint64_t start_cyles = dd_perf_cycle();
        dd_thread_wait_assert_src(waiter, NULL);
        waiter->event.base.wait_src = list;
        waiter->event.base.tag = DD_PTR_2_NUM(elem, dd_uint64_t);
        dd_perf_mutex_in(&dd_perf_ctx,
                         DD_PERF_MUTEX_LOCK,
                         list->adl_stats.trace_id,
                         &list->adl_stats.trace_gen,
                         list->adl_name,
                         0);
        dd_thread_do_wait(waiter);
        dd_perf_out2(&dd_perf_ctx, DD_PERF_MUTEX_LOCK, (int)waiter->id, 1);
        waiter->event.base.tag = 0;
        dd_atomic32_inc(&list->adl_stats.lock_contentions);
        dd_atomic64_add(&list->adl_stats.lock_contention_cyc,
                        TSC_DIFF_NOW(start_cyles));
    } else {
        /* Element is ready to delete */
        dd_assert(elem->adelem_refcnt.fields.aref_mask == 1);
        dd_assert(elem->adelem_refcnt.fields.aref_pin_count == 0);
    }
    dd_assert(((int)dd_atomic32_dec_with_ret(&list->adl_refcnt)) >= 0);
}

/**
 * Check if an element marked for removal is ready to be removed.
 * This must be called before calling adl_remove_elem_wait.
 */
dd_bool_t 
adl_remove_elem_ready(adlist_t *const list,
                      adelem_t *const elem)
{
    UNUSED_PARAMETER(list);
    dd_assert(dd_atomic32_inc_with_ret(&list->adl_refcnt) > 0);
    adl_refcnt_t old;
    old.aref_atomic = elem->adelem_refcnt.aref_atomic;
    dd_assert(old.fields.aref_wait_id == DD_THREAD_WAIT_ID_MAX);
    dd_assert(old.fields.aref_pin_count >= 1);
    dd_assert(old.fields.aref_mask == 1);
    dd_assert(((int)dd_atomic32_dec_with_ret(&list->adl_refcnt)) >= 0);
    return (old.fields.aref_pin_count == 1);
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
    
    dd_assert(dd_atomic32_inc_with_ret(&list->adl_refcnt) > 0);
    first_elem = _adl_first(list);

    while (first_elem != NULL) {
        res = adl_remove_elem_start(list, first_elem);
        switch (res) {
            case ADL_REMOVE_FAIL:
                adl_elem_unpin(list, first_elem);
                first_elem = _adl_first(list);
                break;
            case ADL_REMOVE_SUCCESS:
                dd_assert(((int)dd_atomic32_dec_with_ret(&list->adl_refcnt)) >= 0);
                return first_elem;
            case ADL_REMOVE_SUCCESS_WAIT:
                adl_remove_elem_finish(list, first_elem, NULL);
                dd_assert(((int)dd_atomic32_dec_with_ret(&list->adl_refcnt)) >= 0);
                return first_elem;
            default:
                dd_panic("Unknown remove result %d\n", res);
        }
    }
    
    dd_assert(first_elem == NULL);
    dd_assert(((int)dd_atomic32_dec_with_ret(&list->adl_refcnt)) >= 0);
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
    
    dd_assert(dd_atomic32_inc_with_ret(&list->adl_refcnt) > 0);
    last_elem = _adl_last(list);

    while (last_elem != NULL) {
        res = adl_remove_elem_start(list, last_elem);
        switch (res) {
            case ADL_REMOVE_FAIL:
                adl_elem_unpin(list, last_elem);
                last_elem = _adl_last(list);
                break;
            case ADL_REMOVE_SUCCESS:
                dd_assert(((int)dd_atomic32_dec_with_ret(&list->adl_refcnt)) >= 0);
                return last_elem;
            case ADL_REMOVE_SUCCESS_WAIT:
                adl_remove_elem_finish(list, last_elem, NULL);
                dd_assert(((int)dd_atomic32_dec_with_ret(&list->adl_refcnt)) >= 0);
                return last_elem;
            default:
                dd_panic("Unknown remove result %d\n", res);
        }
    }
    
    dd_assert(last_elem == NULL);
    dd_assert(((int)dd_atomic32_dec_with_ret(&list->adl_refcnt)) >= 0);
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
                    IN dd_bool_t return_pinned)
{
    dd_assert(dd_atomic32_inc_with_ret(&list->adl_refcnt) > 0);
    adl_elem_init(list, new, return_pinned);
    adl_lock_sync(list, &list->adl_head_lock, ADL_LOCK_EXCLUSIVE);
    if (ADL_LIST_HEAD(list) == NULL) {
        adl_lock_sync(list, &list->adl_tail_lock, ADL_LOCK_EXCLUSIVE);
        dd_assert(ADL_LIST_TAIL(list) == NULL);
        ADL_LIST_TAIL_SET(list, new);
        ADL_LIST_HEAD_SET(list, new);
        new->adelem_next = new->adelem_prev = NULL;
        dd_atomic32_inc(&list->adl_count); /* Must update before expose */
        adl_elem_expose(new, return_pinned);
        adl_unlock(list, &list->adl_tail_lock, ADL_LOCK_EXCLUSIVE);
    } else {
        adelem_t *first_elem = ADL_LIST_HEAD(list);
        adl_lock_sync(list, &first_elem->adelem_lock, ADL_LOCK_EXCLUSIVE);
        dd_assert(ADL_ELEM_PREV(first_elem) == NULL);
        dd_assert(ADL_LIST_HEAD(list) == first_elem);
        ADL_ELEM_PREV_SET(first_elem, new);
        new->adelem_prev = NULL;
        new->adelem_next = first_elem;
        ADL_LIST_HEAD_SET(list, new);
        dd_atomic32_inc(&list->adl_count); /* Must update before expose */
        adl_elem_expose(new, return_pinned);
        adl_unlock(list, &first_elem->adelem_lock, ADL_LOCK_EXCLUSIVE);
    }
    adl_unlock(list, &list->adl_head_lock, ADL_LOCK_EXCLUSIVE);
#ifdef DD_DEBUG
    if (return_pinned) {
        adl_assert_elem_is_pinned(new);
        adl_assert_elem_on_list(list, new);
    }
#endif
    dd_assert(((int)dd_atomic32_dec_with_ret(&list->adl_refcnt)) >= 0);
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
                  IN dd_bool_t return_pinned)
{
    int ret;
    dd_assert(dd_atomic32_inc_with_ret(&list->adl_refcnt) > 0);
    adl_elem_init(list, new, return_pinned);
    adl_lock_sync(list, &list->adl_tail_lock, ADL_LOCK_EXCLUSIVE);
retry:
    if (ADL_LIST_TAIL(list) == NULL) {
        ret = adl_lock_async(list, &list->adl_head_lock, ADL_LOCK_EXCLUSIVE);
        if (ret != 0) {
            adl_unlock(list, &list->adl_tail_lock, ADL_LOCK_EXCLUSIVE);
            adl_lock_wait(list, &list->adl_head_lock, ADL_LOCK_EXCLUSIVE);
            adl_lock_sync(list, &list->adl_tail_lock, ADL_LOCK_EXCLUSIVE);
            if (ADL_LIST_TAIL(list) != NULL) {
                /* list changed */
                adl_unlock(list, &list->adl_head_lock, ADL_LOCK_EXCLUSIVE);
                goto retry;
            }
        }
        dd_assert(ADL_LIST_HEAD(list) == NULL);
        ADL_LIST_TAIL_SET(list, new);
        ADL_LIST_HEAD_SET(list, new);
        new->adelem_next = new->adelem_prev = NULL;
        dd_atomic32_inc(&list->adl_count);
        adl_elem_expose(new, return_pinned);
        adl_unlock(list, &list->adl_head_lock, ADL_LOCK_EXCLUSIVE);
    } else {
        adelem_t *last_elem = ADL_LIST_TAIL(list);
        ret = adl_lock_async(list, &last_elem->adelem_lock, ADL_LOCK_EXCLUSIVE);
        if (ret != 0) {
            adl_unlock(list, &list->adl_tail_lock, ADL_LOCK_EXCLUSIVE);
            adl_lock_wait(list, &last_elem->adelem_lock, ADL_LOCK_EXCLUSIVE);
            adl_lock_sync(list, &list->adl_tail_lock, ADL_LOCK_EXCLUSIVE);
            if (ADL_LIST_TAIL(list) != last_elem) {
                /* List changed */
                adl_unlock(list, &last_elem->adelem_lock, ADL_LOCK_EXCLUSIVE);
                goto retry;
            }
        }
        dd_assert(ADL_ELEM_NEXT(last_elem) == NULL);
        ADL_ELEM_NEXT_SET(last_elem, new);
        new->adelem_prev = last_elem;
        new->adelem_next = NULL;
        ADL_LIST_TAIL_SET(list, new);
        dd_atomic32_inc(&list->adl_count);
        adl_elem_expose(new, return_pinned);
        adl_unlock(list, &last_elem->adelem_lock, ADL_LOCK_EXCLUSIVE);
    }
    adl_unlock(list, &list->adl_tail_lock, ADL_LOCK_EXCLUSIVE);
#ifdef DD_DEBUG
    if (return_pinned) {
        adl_assert_elem_is_pinned(new);
        adl_assert_elem_on_list(list, new);
    }
#endif
    dd_assert(((int)dd_atomic32_dec_with_ret(&list->adl_refcnt)) >= 0);
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
                       IN dd_bool_t return_pinned)
{
    int ret;
    adelem_t *prev;
    dd_lwlock_t *prev_lock;
    
    dd_assert(dd_atomic32_inc_with_ret(&list->adl_refcnt) > 0);
    adl_assert_elem_is_pinned(elem);
    adl_assert_elem_on_list(list, elem);

    adl_elem_init(list, new, return_pinned);
    adl_lock_sync(list, &elem->adelem_lock, ADL_LOCK_EXCLUSIVE);
retry:
    prev = ADL_ELEM_PREV(elem);
    if (prev == NULL) {
        /* elem is first element */
        prev_lock = &list->adl_head_lock;
    } else {
        prev_lock = &prev->adelem_lock;
    }

    ret = adl_lock_async(list, prev_lock, ADL_LOCK_EXCLUSIVE);
    if (ret != 0) {
        adl_unlock(list, &elem->adelem_lock, ADL_LOCK_EXCLUSIVE);
        adl_lock_wait(list, prev_lock, ADL_LOCK_EXCLUSIVE);
        adl_lock_sync(list, &elem->adelem_lock, ADL_LOCK_EXCLUSIVE);
        if (ADL_ELEM_PREV(elem) != prev) {
            /* List changed. elem is also possibly deleted by now.
             * This is where we want caller to ensure the elem is
             * not deleted.
             */
            adl_unlock(list, prev_lock, ADL_LOCK_EXCLUSIVE);
            goto retry;
        }
    }
    new->adelem_next = elem;
    new->adelem_prev = prev;
    ADL_ELEM_PREV_SET(elem, new);
    if (prev == NULL) {
        ADL_LIST_HEAD_SET(list, new);
    } else {
        ADL_ELEM_NEXT_SET(prev, new);
    }
    dd_atomic32_inc(&list->adl_count);
    adl_elem_expose(new, return_pinned);
    adl_unlock(list, prev_lock, ADL_LOCK_EXCLUSIVE);
    adl_unlock(list, &elem->adelem_lock, ADL_LOCK_EXCLUSIVE);
    adl_assert_elem_is_pinned(elem);
    adl_assert_elem_on_list(list, elem);
#ifdef DD_DEBUG
    if (return_pinned) {
        adl_assert_elem_is_pinned(new);
        adl_assert_elem_on_list(list, new);
    }
#endif
    dd_assert(((int)dd_atomic32_dec_with_ret(&list->adl_refcnt)) >= 0);
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
                      IN dd_bool_t return_pinned)
{
    adelem_t *next;
    
    dd_assert(dd_atomic32_inc_with_ret(&list->adl_refcnt) > 0);
    adl_assert_elem_on_list(list, elem);
    adl_assert_elem_is_pinned(elem);

    adl_elem_init(list, new, return_pinned);
    
    adl_lock_sync(list, &elem->adelem_lock, ADL_LOCK_EXCLUSIVE);
    next = ADL_ELEM_NEXT(elem);
    new->adelem_prev = elem;
    new->adelem_next = next;
    ADL_ELEM_NEXT_SET(elem, new);
    if (next == NULL) {
        adl_lock_sync(list, &list->adl_tail_lock, ADL_LOCK_EXCLUSIVE);
        ADL_LIST_TAIL_SET(list, new);
        dd_atomic32_inc(&list->adl_count);
        adl_elem_expose(new, return_pinned);
        adl_unlock(list, &list->adl_tail_lock, ADL_LOCK_EXCLUSIVE);
    } else {
        adl_lock_sync(list, &next->adelem_lock, ADL_LOCK_EXCLUSIVE);
        ADL_ELEM_PREV_SET(next, new);
        dd_atomic32_inc(&list->adl_count);
        adl_elem_expose(new, return_pinned);
        adl_unlock(list, &next->adelem_lock, ADL_LOCK_EXCLUSIVE);
    }
    adl_unlock(list, &elem->adelem_lock, ADL_LOCK_EXCLUSIVE);
    adl_assert_elem_is_pinned(elem);
    adl_assert_elem_on_list(list, elem);
#ifdef DD_DEBUG
    if (return_pinned) {
        adl_assert_elem_is_pinned(new);
        adl_assert_elem_on_list(list, new);
    }
#endif
    dd_assert(((int)dd_atomic32_dec_with_ret(&list->adl_refcnt)) >= 0);
}

#define ITER_DONE(iter) ((iter)->current_elem == ADL_DBG_BADELEM)
#define ITER_SET_DONE(iter) (iter)->current_elem = ADL_DBG_BADELEM

/* Initialize an adlist iter */
void 
adlist_iter_init(adlist_iter_t *const iter,
                 adlist_t *const list,
                 IN adl_iter_direction_t direction)
{
    dd_assert(list != NULL);
    iter->list = list;
    iter->direction = direction;
    iter->current_elem = NULL;
    iter->count = 0;
    iter->return_current = FALSE;
#ifdef DD_DEBUG
    iter->leak_check = dd_malloc(1, MOD_UNKNOWN);
#endif
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
#ifdef DD_DEBUG
    dd_free(iter->leak_check);
    iter->leak_check = NULL;
#endif
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
                dd_panic("Unknown iteration direction\n");
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
dd_bool_t
adlist_iter_pop_start(adlist_iter_t *const iter)
{
    adl_remove_result_t remove_result;
    adelem_t *elem = iter->current_elem;
    adlist_t *list = iter->list;
    dd_panic_if(ITER_DONE(iter) || elem == NULL); 
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
    dd_verify(remove_result == ADL_REMOVE_SUCCESS_WAIT);
    return TRUE;
}

void
adlist_iter_pop_finish(adlist_iter_t *const iter)
{
    adelem_t *elem = iter->current_elem;
    adlist_t *list = iter->list;
    adelem_t *next_elem = NULL;
    dd_panic_if(ITER_DONE(iter) || elem == NULL); 
    adl_assert_elem_is_pinned(elem);
    adl_assert_elem_on_list(list, elem);
    dd_assert(elem->adelem_refcnt.fields.aref_pin_count >= 2);
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
        dd_assert(ITER_DONE(iter));
        adl_remove_elem_finish(list, elem, NULL);
        return;
    }
    dd_assert(!ITER_DONE(iter));
    adl_elem_unpin(list, next_elem);
    /* Now wait for go-ahead on elem to delete */
    adl_remove_elem_wait(list, elem, NULL);
    /* Now re-get next_elem */
    if (iter->direction == ADL_ITER_FORWARD) {
        next_elem = adl_next_intern(list, elem, FALSE);
    } else {
        dd_assert(iter->direction == ADL_ITER_BACKWARD);
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
dd_bool_t
adlist_iter_seek(adlist_iter_t *const iter,
                 adelem_t *const elem,
                 dd_bool_t need_pin)
{
    dd_assert(iter->list != NULL);
    dd_assert(iter->leak_check != NULL);
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

/*
 * Functions to initialize/shutdown adlist stats reporting related infrastructure.
 */
static adlist_t _registered_alists;

void
adlist_init(void)
{
    adl_init(&_registered_alists, "Registered contention alists", NULL);
}

void
adlist_shutdown(void)
{
    adl_destroy(&_registered_alists);
}

void
adlist_register(adlist_t *list)
{
    adl_append_at_end(&_registered_alists, &list->adl_register_link, FALSE);
}

void adlist_unregister(adlist_t *list)
{
    dd_verify(adl_elem_delete(&_registered_alists, &list->adl_register_link, TRUE /* Need to pin */));
}

/*
 * Functions to get/print stats for an alist
 */
void
adlist_stats_str(adlist_t *list, char *buf, size_t size, size_t *len)
{
    DD_PRINTBUF_CHK(buf, size, len, "\t%-21.21s ", list->adl_name);
    dd_lwlock_stats_str(&list->adl_stats, buf, size, len);
}

void
adlist_stats_print(adlist_t *list)
{
    char buf[1024];
    size_t pos = 0;
    adlist_stats_str(list, buf, sizeof(buf), &pos);
    dd_dprintf(0, dbgLib, "%s: %s\n", __func__, buf);
}

void
adlist_stats_str_all(char *buf, size_t size, size_t *len)
{
    adlist_iter_t iter;
    adlist_t *list;
    DD_PRINTBUF_CHK(buf, size, len, "\nADList Stats:\n");
    adlist_iter_init(&iter, &_registered_alists, ADL_ITER_FORWARD);
    while ((list = _adlist_iter_next(&iter)) != NULL) {
        adlist_stats_str(list, buf, size, len);
    }
    adlist_iter_destroy(&iter);
}

void
adlist_stats_reset(adlist_t *list)
{
    dd_lwlock_stats_reset(&list->adl_stats);
}

void
adlist_stats_reset_all(void)
{
    adlist_iter_t iter;
    adlist_t *list;
    adlist_iter_init(&iter, &_registered_alists, ADL_ITER_FORWARD);
    while ((list = _adlist_iter_next(&iter)) != NULL) {
        adlist_stats_reset(list);
    }
    adlist_iter_destroy(&iter);
}
