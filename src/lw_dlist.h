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

/*
 * Forward reference for use within a list element structure.
 */
typedef struct lw_dlist_struct lw_dlist_t;

/**
 * Generic List Element structure
 */
typedef struct {
    void *lw_delem_next;    /**< Pointer to the next element in the list */
    void *lw_delem_prev;    /**< Pointer to the previous element in the list */
#ifdef LW_DEBUG
    lw_dlist_t *list;
    lw_magic_t magic;
#endif
} lw_delem_t;


/**
 * Generic List Head structure
 */
struct lw_dlist_struct {
    lw_delem_t *lw_dlist_head;    /**< Pointer to the first element in the list */
    lw_delem_t *lw_dlist_tail;    /**< Pointer to the last element in the list */
    lw_uint32_t lw_dlist_count;   /**< Number of members in the list */
#ifdef LW_DEBUG
    lw_magic_t lw_dlist_magic;
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
extern void *lw_dl_dequeue(LW_INOUT lw_dlist_t *list);

#endif

