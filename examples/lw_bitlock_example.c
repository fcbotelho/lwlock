
#include "lw_lock.h"
#include "lw_lock_common.h"
#include "lw_debug.h"

#include <stdio.h>
/* No need to include pthread.h to use lw_lock libray. Including it
 * here in this test program to be able to use pthread_yield() to
 * increase chance of race
 */
#include <pthread.h>
#include <sched.h>
#define cpu_yield() sched_yield()

#define DATA_NUM    sizeof(lw_uint32_t)
#define THRD_NUM    (2 * DATA_NUM)
#define DATA_MAX_INCREMENT 100

lw_uint64_t data[DATA_NUM];
lw_uint32_t lock = 0;
lw_mutex_t barrier_mutex;

static void
clear_data(void)
{
    lw_uint32_t i;
    for (i = 0; i < DATA_NUM; i++) {
        data[i] = 0;
    }
}

static void
critical_region(lw_uint32_t tid, lw_bool_t signal_only)
{
    lw_uint32_t index = tid % DATA_NUM;
    lw_uint32_t lock_mask = 0x1 << (8 * index);
    lw_uint32_t wait_mask = 0x2 << (8 * index);
    lw_uint32_t cv_mask = 0x4 << (8 * index);
    lw_bitlock32_spec_t spec;
    spec.lock = &lock;
    spec.lock_mask = lock_mask;
    spec.wait_mask = wait_mask;

    lw_lock_common_acquire_lock(&spec, LW_LOCK_TYPE_BITLOCK32, NULL);

    lw_bitlock32_cv_signal(&lock, lock_mask, wait_mask, cv_mask);
    if (!signal_only) {
        data[index] += 1;
        lw_bitlock32_cv_wait(&lock, lock_mask, wait_mask, cv_mask);
    }

    lw_lock_common_drop_lock(&spec, LW_LOCK_TYPE_BITLOCK32);
}

static void *
wr_thread_fn(void *arg)
{
    lw_uint32_t i;
    lw_uint32_t tid = LW_PTR_2_NUM(arg, tid);

    // wait for all threads to be created
    lw_mutex_lock(&barrier_mutex);
    lw_mutex_unlock(&barrier_mutex);

    for (i = 0; i < DATA_MAX_INCREMENT; i++) {
        critical_region(tid, FALSE);
    }
    critical_region(tid, TRUE);
    return NULL;
}

static void
do_test(void)
{
    lw_uint32_t i;
    pthread_t wr_thrds[THRD_NUM];

    fprintf(stdout, "RUNNING TEST\n");
    fprintf(stdout, "------------------------------\n");

    clear_data();
    lw_verify(THRD_NUM % DATA_NUM == 0);

    lw_mutex_lock(&barrier_mutex);

    /* create threads */
    for (i = 0; i < THRD_NUM; i++) {
        lw_verify(pthread_create(&wr_thrds[i],
                                 NULL,
                                 wr_thread_fn,
                                 LW_NUM_2_PTR(i, void *)) == 0);
        fprintf(stdout, "%s: created write thread %d\n", __func__, i);
    }

    /* allow the threads to actually start working */
    lw_mutex_unlock(&barrier_mutex);

    /* join the threads */
    for (i = 0; i < THRD_NUM; i++) {
        pthread_join(wr_thrds[i], NULL);
        fprintf(stdout, "%s: joined write thread %d\n", __func__, i);
    }

    fprintf(stdout, "Final values:\n");

    for (i = 0; i < DATA_NUM; i++) {
        lw_verify(data[i] == DATA_MAX_INCREMENT * (THRD_NUM / DATA_NUM));
        fprintf(stdout, "\tdata[%d] = %llu\n", i, data[i]);
    }
}

int main(int argc, char **argv)
{
    /* common init */
    lw_lock_init(NULL);
    lw_mutex_init(&barrier_mutex);

    do_test();

    /* common shutdown */
    lw_mutex_destroy(&barrier_mutex);
    lw_lock_shutdown();
    return 0;
}
