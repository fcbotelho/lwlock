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

void lw_dl_init(LW_INOUT lw_dlist_t *list)
{
#ifdef LW_DEBUG
    list->lw_dlist_magic = LW_DL_INITIALIZED;
#endif
    list->lw_dlist_head = list->lw_dlist_tail = NULL;
    list->lw_dlist_count = 0;
}

void lw_dl_destroy(LW_INOUT lw_dlist_t *list)
{
#ifdef LW_DEBUG
    list->lw_dlist_magic = 0;
#endif
    list->lw_dlist_head = list->lw_dlist_tail = NULL;
    list->lw_dlist_count = 0;
}

void lw_dl_init_elem(LW_INOUT lw_delem_t *elem)
{
    elem->lw_delem_next = elem->lw_delem_prev = LW_DL_DBG_BADELEM;
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

    if (list->lw_dlist_head == NULL) {
        list->lw_dlist_head = list->lw_dlist_tail = elem;
        elem->lw_delem_next = elem->lw_delem_prev = NULL;
    } else {
        elem->lw_delem_prev = list->lw_dlist_tail;
        elem->lw_delem_next = NULL;
        list->lw_dlist_tail->lw_delem_next = elem;
        list->lw_dlist_tail = elem;
    }

    list->lw_dlist_count++;

#ifdef LW_DEBUG
    elem->list = list;
    elem->magic = LW_DL_ON_LIST;
#endif
}

void *lw_dl_dequeue(LW_INOUT lw_dlist_t *list)
{
    lw_delem_t *elem;
    lw_delem_t *n;

    elem = list->lw_dlist_head;

    if (elem != NULL) {
#ifdef LW_DEBUG
        lw_assert((elem->magic == LW_DL_ON_LIST) &&
                  (elem->list == list));
#endif
        lw_assert(elem->lw_delem_prev == NULL);

        n = elem->lw_delem_next;

        if (n != NULL) {
            lw_assert(n->lw_delem_prev == elem);
            n->lw_delem_prev = NULL;
        } else {
            list->lw_dlist_tail = NULL;
        }
        list->lw_dlist_head = n;

        lw_verify(list->lw_dlist_count > 0);
        list->lw_dlist_count--;

        /*
         * Reset the link fields within the element
         */
        lw_dl_init_elem(elem);
    } else {
        lw_assert(list->lw_dlist_tail == NULL);
        lw_verify(list->lw_dlist_count == 0);
    }

    return elem;
}

