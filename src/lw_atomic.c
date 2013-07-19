#include "lw_atomic.h"
#include <pthread.h>

#if !defined(AMD64_ASM)

pthread_mutex_t lw_atomic_mutex;

static void lw_atomic_init(void)
{
    pthread_mutex_init(&lw_atomic_mutex, NULL);
}

lw_uint32_t lw_uint32_cmpxchg(volatile lw_uint32_t *var, lw_uint32_t old, lw_uint32_t new)
{
    lw_uint32_t ret;

    lw_panic_if(pthread_mutex_lock(&lw_atomic_mutex) != 0);
    ret = *var;
    if (*var == old) {
        *var = new;
    }
    lw_panic_if(pthread_mutex_unlock(&lw_atomic_mutex) != 0);

    return ret;
}

lw_uint64_t lw_uint64_cmpxchg(volatile lw_uint64_t *var, lw_uint64_t old, lw_uint64_t new)
{
    lw_uint64_t ret;

    lw_panic_if(pthread_mutex_lock(&lw_atomic_mutex) != 0);
    ret = *var;
    if (*var == old) {
        *var = new;
    }
    lw_panic_if(pthread_mutex_unlock(&lw_atomic_mutex) != 0);

    return ret;
}

#endif



