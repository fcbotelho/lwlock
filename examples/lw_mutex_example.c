#include "lw_lock.h"
#include "lw_debug.h"

#include <stdio.h>
/* No need to include pthread.h to use lw_lock libray. Including it
 * here in this test program to be able to use pthread_yiled() to
 * increase chance of race 
 */
#include <pthread.h> 


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
lw_mutex_t data_mutex; // it protects the data (if used)

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
critical_region(lw_bool_t with_lock) 
{
    lw_uint32_t i;
    if (with_lock == TRUE) {
        lw_mutex_lock(&data_mutex, NULL);
    }

    for (i = 0; i < DATA_NUM; i++) {
        data[i] += 1;
        pthread_yield(); // increase chance of race
    }

    for (i = 0; i < DATA_NUM; i++) {
        sum += data[i];
    }

    if (with_lock == TRUE) {
        lw_mutex_unlock(&data_mutex, TRUE);
    }
}

static void *
test_thread_fn(void *arg)
{
    lw_uint32_t i;
    lw_bool_t use_lock = *((lw_bool_t *) arg);

    // wait for all threads to be created
    lw_mutex_lock(&barrier_mutex, NULL);
    lw_mutex_unlock(&barrier_mutex, TRUE);

    for (i =0; i < DATA_MAX_INCREMENT; i++) {
        critical_region(use_lock);
    }

    return NULL;
}

static void
do_test(lw_bool_t use_lock)
{
    lw_uint32_t i;
    lw_thread_t thrds[THRD_NUM];
    
    fprintf(stdout, "%s: RUNNING TEST (use_lock=%s) \n", __func__, use_lock ? "TRUE" : "FALSE");
    fprintf(stdout, "%s:------------------------------\n", __func__);

    clear_data();

    lw_lock_init(TRUE);
    lw_mutex_init(&data_mutex);
    lw_mutex_init(&barrier_mutex);

    lw_mutex_lock(&barrier_mutex, NULL);
    for (i = 0; i < THRD_NUM; i++) {
        lw_verify(lw_thread_create(&thrds[i], 
                                   NULL, 
                                   test_thread_fn, 
                                   (void *)&use_lock, 
                                   "test_thrd") == 0);
        fprintf(stdout, "%s: created thread %d\n", __func__, i);
    }
    lw_mutex_unlock(&barrier_mutex, TRUE); // This will allow the threads to actually start working

    for (i = 0; i < THRD_NUM; i++) {
        lw_thread_join(thrds[i], NULL);
        fprintf(stdout, "%s: joined thread %d\n", __func__, i);
    }

    fprintf(stdout, "%s: Final values:\n", __func__);

    for (i = 0; i < DATA_NUM; i++) {
        fprintf(stdout, "\tdata[%d] = %llu\n", i, data[i]);
    }
    fprintf(stdout, "\tsum = %llu\n", sum);

    lw_mutex_destroy(&data_mutex);
    lw_mutex_destroy(&barrier_mutex);
    lw_lock_shutdown();
}


int main(int argc, char **argv)
{
    do_test(TRUE); // test with our lock, this should produce correct final value
    do_test(FALSE); // test without lock, this should produce incorrect final value
    return 0;
}
