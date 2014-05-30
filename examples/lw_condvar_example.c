#include "lw_lock.h"
#include "lw_lock_common.h"
#include "lw_debug.h"

#include <stdio.h>

/* A producer-consumer implementation that works with
 * lw mutex and condvar
 */

#define BUFF_SIZE 10
#define CONSUMERS 6
#define PRODUCERS 6
#define LOOP_COUNT 100

int buff[BUFF_SIZE];
int in_idx = 0;
int out_idx = 0;

/* mutex and convar for producer consumer logic */
lw_mutex_t mutex;
lw_condvar_t condvar_newdata;
lw_condvar_t condvar_newspace;

lw_mutex_t barrier_mutex; //used to make sure that threads start simultaneously

int global_count = 0; // used to generate new values for the producers to produce
int global_sum = 0; // conumers will add the consumed values to it

void *
consumer_func(void *arg)
{
    int cons_id = *((int *) arg);
    int i = 0;

    // wait for all threads to be created
    lw_mutex_lock(&barrier_mutex);
    lw_mutex_unlock(&barrier_mutex);

    fprintf(stdout, "Consumer thread %d starts\n", cons_id);
    for(i = 0;i < LOOP_COUNT; i++) {
        lw_mutex_lock(&mutex);
        while(in_idx == out_idx) {
            lw_condvar_wait(&condvar_newdata,
                            &mutex,
                            LW_LOCK_TYPE_LWMUTEX);
        }
        fprintf(stdout, "Consumer %d consumed: %d\n", cons_id, buff[out_idx]);
        global_sum += buff[out_idx];
        out_idx = (out_idx + 1) % BUFF_SIZE;
        lw_condvar_signal(&condvar_newspace);
        lw_mutex_unlock(&mutex);
    }
    fprintf(stdout, "Consumer thread %d exits\n", cons_id);
    return NULL;
}

void *
producer_func(void *arg)
{
    int prod_id = *((int *) arg);
    int i = 0;

    // wait for all threads to be created
    lw_mutex_lock(&barrier_mutex);
    lw_mutex_unlock(&barrier_mutex);

    fprintf(stdout, "Producer thread %d starts\n", prod_id);
    for(i = 0; i < LOOP_COUNT; i++) {
        lw_mutex_lock(&mutex);
        while(((in_idx + 1) % BUFF_SIZE) == out_idx) {
            lw_condvar_wait(&condvar_newspace,
                            &mutex,
                            LW_LOCK_TYPE_LWMUTEX);
        }
        buff[in_idx] = ++global_count;
        fprintf(stdout, "Producer %d produced: %d\n", prod_id, global_count);
        in_idx = (in_idx + 1) % BUFF_SIZE;
        lw_condvar_signal(&condvar_newdata);
        lw_mutex_unlock(&mutex);
    }
    fprintf(stdout, "Producer thread %d exits\n", prod_id);
    return NULL;
}

int main(int argc, char **argv)
{

    int prod_args[CONSUMERS];
    int cons_args[PRODUCERS];
    pthread_t cons_thrds[CONSUMERS];
    pthread_t prod_thrds[PRODUCERS];
    int i;

    lw_lock_init(NULL, 0, NULL, 0);

    lw_condvar_init(&condvar_newdata);
    lw_condvar_init(&condvar_newspace);
    lw_mutex_init(&mutex);
    lw_mutex_init(&barrier_mutex);

    lw_mutex_lock(&barrier_mutex);

    /* create producer threads */
    for (i = 0; i < PRODUCERS; i++) {
        prod_args[i] = i;
        lw_verify(pthread_create(&prod_thrds[i],
                                 NULL,
                                 producer_func,
                                 &prod_args[i]) == 0);
        fprintf(stdout, "created producer thread %d\n", i);
    }

    /* create consumer threads */
    for (i = 0; i < CONSUMERS; i++) {
        cons_args[i] = i;
        lw_verify(pthread_create(&cons_thrds[i],
                                 NULL,
                                 consumer_func,
                                 &cons_args[i]) == 0);
        fprintf(stdout, "created consumer thread %d\n", i);
    }

    lw_mutex_unlock(&barrier_mutex); // This will allow the threads to actually start working


    /* join producer threads */
    for (i = 0; i < PRODUCERS; i++) {
        pthread_join(prod_thrds[i], NULL);
        fprintf(stdout, "joined producer thread %d\n", i);
    }

    /* join consumer threads */
    for (i = 0; i < CONSUMERS; i++) {
        pthread_join(cons_thrds[i], NULL);
        fprintf(stdout, "joined consumer thread %d\n", i);
    }

    fprintf(stdout, "------------------------------\n");
    fprintf(stdout, "Final global_count = %d\n", global_count);
    fprintf(stdout, "Final global_sum = %d\n", global_sum);
    fprintf(stdout,
            "[global_sum=(global_count+1)*global_count/2=(%d+1)*%d/2=%d]\n",
            global_count,
            global_count,
            (global_count + 1) * global_count / 2);

    lw_mutex_destroy(&mutex);
    lw_condvar_destroy(&condvar_newdata);
    lw_condvar_destroy(&condvar_newspace);


    lw_lock_shutdown();
    return 0;
}
