#include "lw_lock.h"
#include "lw_lock_common.h"
#include "lw_debug.h"

#include <stdio.h>

/* This program does nothing, just created it to see that everything
 * compiles and links. If executed, the program will show undefined
 * behavior. Need to rewrite the program to actually do something 
 */

int main(int argc, char **argv) 
{
    lw_lock_init(TRUE);

    lw_condvar_t lwcondvar;
    lw_mutex_t lwmutex;

    lw_condvar_init(&lwcondvar);
    lw_mutex_init(&lwmutex);
    lw_condvar_wait(&lwmutex, LW_LOCK_TYPE_LWMUTEX, NULL, &lwcondvar); 
    lw_mutex_destroy(&lwmutex);
    lw_condvar_init(&lwcondvar);

    lw_lock_shutdown();
    return 0;
}
