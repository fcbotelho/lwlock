#ifndef __LW_DLIST_H__
#define __LW_DLIST_H__


/*
 * Generic Linear Doubly Linked List Support.
 */

#define DL_DBG_BADELEM   ((delem_t *)0xdeadbeef)

/*
 * Forward reference for use within a list element structure.
 */
typedef struct dlist_struct dlist_t;

/**
 * Generic List Element structure
 */
typedef struct {
    void *next;    /**< Pointer to the next element in the list */
    void *prev;    /**< Pointer to the previous element in the list */
} delem_t;


/**
 * Generic List Head structure
 */
struct dlist_struct {
    delem_t *head;        /**< Pointer to the first element in the list */
    delem_t *tail;        /**< Pointer to the last element in the list */
    dd_uint32_t count;    /**< Number of members in the list */
};

/**
 * Initialize a list, preparing it for use.
 *
 * @param list (i/o) the list to be initialized.
 */
static inline void dl_init(dlist_t *list)
{
    list->head = list->tail = NULL;
    list->count = 0;
}

/**
 * Destroy a list, ensuring it cannot be used again.
 *
 * @param list (i/o) the list to be initialized.
 */
static inline void dl_destroy(dlist_t *list)
{
    list->head = list->tail = NULL;
    list->count = 0;
}

/**
 * Mark the element so that it is easily and reliably recognizable as
 * an orphan.
 *
 * @param elem (i/o) the list element to be marked as an orphan.
 */
static inline void dl_init_elem(delem_t *elem)
{
    elem->next = elem->prev = DL_DBG_BADELEM;
}

/**
 * Append the specified element at the end of the specified list.
 *
 * @param list (i/o) the list to which the element is to be appended.
 * @param elem (i/o) the list element to be inserted.
 */
static inline void dl_append_at_end(dlist_t *list, delem_t *elem)
{

    if (list->head == NULL) {
        list->head = list->tail = elem;
        elem->next = elem->prev = NULL;
    } else {
        elem->prev = list->tail;
        elem->next = NULL;
        list->tail->next = elem;
        list->tail = elem;
    }

    list->count++;

}

/**
 * Remove the first element from the specified list.
 *
 * @param list (i) the list from which the element is to be
 *     removed.
 *
 * @return A pointer to the first element in the list if the list is
 *     not empty or a NULL pointer if the list is empty.
 */
static inline void *_dl_dequeue(dlist_t *list)
{
    delem_t *elem;
    delem_t *n;

    elem = list->head;

    if (elem != NULL) {
        dd_assert(elem->prev == NULL);

        n = elem->next;

        if (n != NULL) {
            dd_assert(n->prev == elem);
            n->prev = NULL;
        } else {
            list->tail = NULL;
        }
        list->head = n;

        lw_verify(list->count > 0);
        list->count--;

        /*
         * Reset the link fields within the element
         */
        dl_init_elem(elem);
    } else {
        lw_assert(list->tail == NULL);
        lw_verify(list->count == 0);
    }

    return elem;
}


#endif

