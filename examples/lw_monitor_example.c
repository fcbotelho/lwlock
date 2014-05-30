
#include "lw_lock.h"
#include "lw_lock_common.h"
#include "lw_debug.h"
#include "lw_monitor.h"

#include <stdio.h>
/* No need to include pthread.h to use lw_lock libray. Including it
 * here in this test program to be able to use pthread_yield() to
 * increase chance of race
 */
#include <pthread.h>
#include <sched.h>
#define cpu_yield() sched_yield()

#define DATA_NUM    4
#define THRD_NUM    16
#define DATA_MAX_INCREMENT 100

lw_uint64_t data[DATA_NUM];
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
critical_region(lw_uint32_t tid)
{
    lw_uint32_t index = tid % DATA_NUM;
    lw_uint32_t num_contenders = THRD_NUM / DATA_NUM;
    lw_uint32_t wait_for = tid / DATA_NUM;

    MONITOR_LOCK_START(&data[index]);
    MONITOR_WAIT(data[index] % num_contenders == wait_for);
    data[index] += 1;
    MONITOR_BROADCAST;
    MONITOR_LOCK_END;
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
        critical_region(tid);
    }
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
    lw_lock_init(NULL, 0, NULL, DATA_NUM + THRD_NUM);
    lw_mutex_init(&barrier_mutex);

    do_test();

    /* common shutdown */
    lw_mutex_destroy(&barrier_mutex);
    lw_lock_shutdown();
    return 0;
}
