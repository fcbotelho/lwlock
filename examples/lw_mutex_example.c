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

#define DATA_NUM 5
#define DATA_MAX_INCREMENT 100
#define THRD_NUM 10

/* We initialize data[] and sum with 0. Then we create THRD_NUM
 * threads. Each thread does this DATA_MAX_INCREMENT times: increment
 * each member of data[] by 1 and then add the summation of current
 * values of data[] to the current value of 'sum'. Each interation is
 * thus a critical region and needs to be protected with a mutex. If
 * the mutex works, at the end of DATA_MAX_INCREMENT iterations each
 * member of data[] should have a value of DATA_MAX_INCREMENT*THRD_NUM
 * and 'sum' should be DATA_NUM*{n*(n+1)/2} where
 * n=DATA_MAX_INCREMENT*THRD_NUM
 */
lw_uint64_t data[DATA_NUM];
lw_uint64_t sum;
void *data_lock; // protects data, use void * to be able to use different kinds of locks
lw_lock_type_t data_lock_type = LW_LOCK_TYPE_NONE;

lw_mutex_t barrier_mutex; //used to make sure that threads start simultaneously

static void
clear_data(void)
{
    lw_uint32_t i;
    for (i = 0; i < DATA_NUM; i++) {
        data[i] = 0;
    }
    sum = 0;
}

static void
critical_region(void)
{
    lw_uint32_t i;

    lw_lock_common_acquire_lock(data_lock, data_lock_type, NULL);

    for (i = 0; i < DATA_NUM; i++) {
        data[i] += 1;
        cpu_yield(); // increase chance of race
    }

    for (i = 0; i < DATA_NUM; i++) {
        sum += data[i];
    }

    lw_lock_common_drop_lock(data_lock, data_lock_type);
}

static void *
wr_thread_fn(void *arg)
{
    lw_uint32_t i;

    // wait for all threads to be created
    lw_mutex_lock(&barrier_mutex);
    lw_mutex_unlock(&barrier_mutex);

    for (i = 0; i < DATA_MAX_INCREMENT; i++) {
        critical_region();
    }

    return NULL;
}

static void
readonly_region(void)
{
    lw_uint32_t i;

    lw_verify (data_lock_type == LW_LOCK_TYPE_LWRWLOCK_WR);

    lw_lock_common_acquire_lock(data_lock, LW_LOCK_TYPE_LWRWLOCK_RD, NULL);

    for (i = 1; i < DATA_NUM; i++) {
        /* all data[i] values should be indentical at a consistent point */
        lw_verify(data[i] == data[0]);
        cpu_yield(); // increase chance of race
    }

    lw_lock_common_drop_lock(data_lock, LW_LOCK_TYPE_LWRWLOCK_RD);
}

static void *
rd_thread_fn(void *arg)
{
    lw_uint32_t i;

    // wait for all threads to be created
    lw_mutex_lock(&barrier_mutex);
    lw_mutex_unlock(&barrier_mutex);

    for (i = 0; i < DATA_MAX_INCREMENT; i++) {
        readonly_region();
    }

    return NULL;
}

static void
do_test(void)
{
    lw_uint32_t i;
    pthread_t wr_thrds[THRD_NUM];
    pthread_t rd_thrds[THRD_NUM]; // only used for testing lw_rwlock_t

    fprintf(stdout,
            "RUNNING TEST (lock type=%s) \n",
            lw_lock_common_lock_type_description(data_lock_type));
    fprintf(stdout, "------------------------------\n");

    clear_data();

    lw_mutex_lock(&barrier_mutex);

    /* create threads */
    for (i = 0; i < THRD_NUM; i++) {
        lw_verify(pthread_create(&wr_thrds[i],
                                 NULL,
                                 wr_thread_fn,
                                 NULL) == 0);
        fprintf(stdout, "%s: created write thread %d\n", __func__, i);
    }
    if (data_lock_type == LW_LOCK_TYPE_LWRWLOCK_WR) {
        for (i = 0; i < THRD_NUM; i++) {
            lw_verify(pthread_create(&rd_thrds[i],
                                     NULL,
                                     rd_thread_fn,
                                     NULL) == 0);
            fprintf(stdout, "%s: created read thread %d\n", __func__, i);
        }
    }

    /* allow the threads to actually start working */
    lw_mutex_unlock(&barrier_mutex);

    /* join the threads */
    for (i = 0; i < THRD_NUM; i++) {
        pthread_join(wr_thrds[i], NULL);
        fprintf(stdout, "%s: joined write thread %d\n", __func__, i);
    }
    if (data_lock_type == LW_LOCK_TYPE_LWRWLOCK_WR) {
        for (i = 0; i < THRD_NUM; i++) {
            pthread_join(rd_thrds[i], NULL);
            fprintf(stdout, "%s: joined read thread %d\n", __func__, i);
        }
    }

    fprintf(stdout, "Final values:\n");

    for (i = 0; i < DATA_NUM; i++) {
        fprintf(stdout, "\tdata[%d] = %llu\n", i, data[i]);
    }
    fprintf(stdout, "\tsum = %llu\n", sum);
}


static void
print_expected_result(void)
{
    lw_uint32_t i;
    lw_uint64_t N = DATA_MAX_INCREMENT * THRD_NUM;

    fprintf(stdout, "Expected Final values:\n");

    for (i = 0; i < DATA_NUM; i++) {
        fprintf(stdout, "\tdata[%d] = %llu\n", i, N);
    }

    fprintf(stdout, "\tsum = %llu\n", DATA_NUM * (N + 1) * N / 2);
}

int main(int argc, char **argv)
{
    /* common init */
    lw_lock_init(NULL, 0, NULL, 0);
    lw_mutex_init(&barrier_mutex);

    print_expected_result();

    /* Test with no lock */
    data_lock = NULL;
    data_lock_type = LW_LOCK_TYPE_NONE;
    do_test();

    /* Test with lw_mutex_t */
    lw_mutex_t mutex;
    lw_mutex_init(&mutex);
    data_lock = &mutex;
    data_lock_type = LW_LOCK_TYPE_LWMUTEX;
    do_test();
    lw_mutex_destroy(&mutex);

    /* Test with lw_mutex2b_t */
    lw_mutex2b_t mutex2b;
    lw_mutex2b_init(&mutex2b);
    data_lock = &mutex2b;
    data_lock_type = LW_LOCK_TYPE_LWMUTEX2B;
    do_test();
    lw_mutex2b_destroy(&mutex2b);

    /* Test with lw_rwlock_t */
    lw_rwlock_t rwlock;
    lw_rwlock_init(&rwlock, LW_RWLOCK_DEFAULT);
    data_lock = &rwlock;
    data_lock_type = LW_LOCK_TYPE_LWRWLOCK_WR;
    do_test();
    lw_rwlock_destroy(&rwlock);

    /* common shutdown */
    lw_mutex_destroy(&barrier_mutex);
    lw_lock_shutdown();
    return 0;
}
