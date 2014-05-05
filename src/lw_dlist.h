/***
 * Developed originally at EMC Corporation, this library is released under the
 * MPL 2.0 license.  Please refer to the MPL-2.0 file in the repository for its
 * full description or to http://www.mozilla.org/MPL/2.0/ for the online version.
 *
 * Before contributing to the project one needs to sign the committer agreement
 * available in the "committerAgreement" directory.
 */

#ifndef __LW_DLIST_H__
#define __LW_DLIST_H__

#include "lw_debug.h"
#include "lw_magic.h"
/*
 * Generic Linear Doubly Linked List Support.
 */

#define LW_DL_DBG_BADELEM   ((lw_delem_t *)0xdeadbeef)
#define LW_DL_DBG_BADLIST   ((lw_dlist_t *)0xfeedface)

typedef enum {
    LW_DL_ON_LIST     = LW_MAGIC(0x1102), /**< lw_delem_t value when an element is on a list */
    LW_DL_OFF_LIST    = LW_MAGIC(0x0913), /**< lw_delem_t value when an element is off lists */
    LW_DL_INITIALIZED = LW_MAGIC(0x0627), /**< lw_dlist_t value when it has been initialized */
} lw_dl_magic_t;

/*
 * Forward reference for use within a list element structure.
 */
typedef struct lw_dlist_struct lw_dlist_t;

/**
 * Generic List Element structure
 */
typedef struct {
    void *next;    /**< Pointer to the next element in the list */
    void *prev;    /**< Pointer to the previous element in the list */
#ifdef LW_DEBUG
    lw_dlist_t *list;
    lw_dl_magic_t magic;
#endif
} lw_delem_t;


/**
 * Generic List Head structure
 */
struct lw_dlist_struct {
    lw_delem_t *head;    /**< Pointer to the first element in the list */
    lw_delem_t *tail;    /**< Pointer to the last element in the list */
    lw_uint32_t count;   /**< Number of members in the list */
    lw_uint32_t _lock;   /**< Treated as lw_rwlock_t for locking. */
#ifdef LW_DEBUG
    lw_dl_magic_t lw_dlist_magic;
#endif
};

/**
 * Initialize a list, preparing it for use.
 *
 * @param list (i/o) the list to be initialized.
 */
extern void lw_dl_init(LW_INOUT lw_dlist_t *list);

/**
 * Destroy a list, ensuring it cannot be used again.
 *
 * @param list (i/o) the list to be initialized.
 */
extern void lw_dl_destroy(LW_INOUT lw_dlist_t *list);

/**
 * Mark the element so that it is easily and reliably recognizable as
 * an orphan.
 *
 * @param elem (i/o) the list element to be marked as an orphan.
 */
extern void lw_dl_init_elem(LW_INOUT lw_delem_t *elem);


/**
 * Append the specified element at the end of the specified list.
 *
 * @param list (i/o) the list to which the element is to be appended.
 * @param elem (i/o) the list element to be inserted.
 */
extern void
lw_dl_append_at_end(LW_INOUT lw_dlist_t *list,
                    LW_INOUT lw_delem_t *elem);

/**
 * Remove the first element from the specified list.
 *
 * @param list (i/o) the list from which the element is to be
 *        removed.
 *
 * @return A pointer to the first element in the list if the list is
 *         not empty or a NULL pointer if the list is empty.
 */
extern lw_delem_t *lw_dl_dequeue(LW_INOUT lw_dlist_t *list);

extern lw_uint32_t lw_dl_get_count(LW_IN lw_dlist_t *list);
extern lw_delem_t *lw_dl_next(LW_IN lw_dlist_t *list, lw_delem_t *elem);
extern lw_delem_t *lw_dl_prev(LW_IN lw_dlist_t *list, lw_delem_t *elem);
extern void lw_dl_insert_before(LW_INOUT lw_dlist_t *list, lw_delem_t *existing, lw_delem_t *new);
extern void lw_dl_insert_after(LW_INOUT lw_dlist_t *list, lw_delem_t *existing, lw_delem_t *new);
extern void lw_dl_remove(LW_INOUT lw_dlist_t *list, lw_delem_t *elem);

extern void lw_dl_lock_writer(LW_INOUT lw_dlist_t *list);
extern void lw_dl_unlock_writer(LW_INOUT lw_dlist_t *list);
extern lw_int32_t lw_dl_trylock_writer(LW_INOUT lw_dlist_t *list);
extern void lw_dl_lock_reader(LW_INOUT lw_dlist_t *list);
extern void lw_dl_unlock_reader(LW_INOUT lw_dlist_t *list);
extern lw_int32_t lw_dl_trylock_reader(LW_INOUT lw_dlist_t *list);

#endif

