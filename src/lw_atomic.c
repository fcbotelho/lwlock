/***
 * Developed originally at EMC Corporation, this library is released under the
 * MPL 2.0 license.  Please refer to the MPL-2.0 file in the repository for its
 * full description or to http://www.mozilla.org/MPL/2.0/ for the online version.
 *
 * Before contributing to the project one needs to sign the committer agreement
 * available in the "committerAgreement" directory.
 */

#include "lw_atomic.h"
#include "lw_debug.h"
#include <pthread.h>

#ifndef AMD64_ASM

pthread_mutex_t lw_atomic_mutex;

void lw_atomic_init(void)
{
    pthread_mutex_init(&lw_atomic_mutex, NULL);
}

void lw_atomic_destroy(void)
{
    pthread_mutex_destroy(&lw_atomic_mutex);
}

lw_uint32_t
lw_uint16_cmpxchg(LW_INOUT volatile lw_uint16_t *var,
                  LW_IN lw_uint16_t old,
                  LW_IN lw_uint16_t new)
{
    lw_uint16_t ret;

    lw_verify(pthread_mutex_lock(&lw_atomic_mutex) == 0);
    ret = *var;
    if (*var == old) {
        *var = new;
    }
    lw_verify(pthread_mutex_unlock(&lw_atomic_mutex) == 0);

    return ret;
}


lw_uint32_t lw_uint32_cmpxchg(LW_INOUT volatile lw_uint32_t *var,
                              LW_IN lw_uint32_t old,
                              LW_IN lw_uint32_t new)
{
    lw_uint32_t ret;

    lw_verify(pthread_mutex_lock(&lw_atomic_mutex) == 0);
    ret = *var;
    if (*var == old) {
        *var = new;
    }
    lw_verify(pthread_mutex_unlock(&lw_atomic_mutex) == 0);

    return ret;
}

lw_uint64_t lw_uint64_cmpxchg(LW_INOUT volatile lw_uint64_t *var,
                              LW_IN lw_uint64_t old,
                              LW_IN lw_uint64_t new)
{
    lw_uint64_t ret;

    lw_verify(pthread_mutex_lock(&lw_atomic_mutex) == 0);
    ret = *var;
    if (*var == old) {
        *var = new;
    }
    lw_verify(pthread_mutex_unlock(&lw_atomic_mutex) == 0);

    return ret;
}

#endif


