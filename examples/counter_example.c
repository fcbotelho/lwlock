#include "lw_lock.h"
#include "lw_lock_common.h"
#include "lw_debug.h"
#include "counter.h"

#include <stdio.h>
/* No need to include pthread.h to use lw_lock libray. Including it
 * here in this test program to be able to use pthread_yield() to
 * increase chance of race
 */
#include <pthread.h>
#include <sched.h>
#define cpu_yield() sched_yield()

#define DATA_NUM    4
#define THRD_NUM    12

#define ROUNDS      1000

typedef enum {
    ALLOC_SYNC = 0,
    ALLOC_TRY = 1,
    ALLOC_ASYNC = 2
} test_alloc_type_t;

static counter_t counter;

lw_mutex_t barrier_mutex;

static void
critical_region(lw_uint32_t tid)
{
    test_alloc_type_t op = tid / DATA_NUM;
    counter_async_alloc_ctx_t ctx;

    switch (op) {
        case ALLOC_SYNC:
            counter_alloc(&counter, 1);
            counter_free(&counter, 1);
            break;
        case ALLOC_TRY:
            if (counter_try_alloc(&counter, 1)) {
                counter_free(&counter, 1);
            }
            break;
        case ALLOC_ASYNC:
            if (counter_alloc_async(&counter, 1, &ctx)) {
                /* Alloc done. release. */
                counter_free(&counter, 1);
            } else if (!counter_alloc_async_cancel(&counter, &ctx)) {
                counter_async_alloc_finish_wait(&counter, &ctx);
                counter_free(&counter, 1);
            }
            break;
        default:
            lw_verify(FALSE);
    }
}

static void *
thread_fn(void *arg)
{
    lw_uint32_t i;
    lw_uint32_t tid = LW_PTR_2_NUM(arg, tid);

    // wait for all threads to be created
    lw_mutex_lock(&barrier_mutex);
    lw_mutex_unlock(&barrier_mutex);

    for (i = 0; i < ROUNDS; i++) {
        critical_region(tid);
    }
    return NULL;
}

static void
do_test(void)
{
    lw_uint32_t i;
    pthread_t thrds[THRD_NUM];

    fprintf(stdout, "RUNNING TEST\n");
    fprintf(stdout, "------------------------------\n");

    lw_verify(THRD_NUM % DATA_NUM == 0);

    lw_mutex_lock(&barrier_mutex);

    /* create threads */
    for (i = 0; i < THRD_NUM; i++) {
        lw_verify(pthread_create(&thrds[i],
                                 NULL,
                                 thread_fn,
                                 LW_NUM_2_PTR(i, void *)) == 0);
        fprintf(stdout, "%s: created write thread %d\n", __func__, i);
    }

    /* allow the threads to actually start working */
    lw_mutex_unlock(&barrier_mutex);

    /* join the threads */
    for (i = 0; i < THRD_NUM; i++) {
        pthread_join(thrds[i], NULL);
        fprintf(stdout, "%s: joined write thread %d\n", __func__, i);
    }
}

int main(int argc, char **argv)
{
    /* common init */
    lw_lock_init(NULL, 0, NULL, 0);
    lw_mutex_init(&barrier_mutex);
    counter_init(&counter, DATA_NUM, TRUE);

    do_test();

    counter_deinit(&counter);

    /* common shutdown */
    lw_mutex_destroy(&barrier_mutex);
    lw_lock_shutdown();
    return 0;
}
