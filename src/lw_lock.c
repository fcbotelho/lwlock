#include "lw_type.h"
#include <dd_atomic.h>

void lw_lock_init(void)
{
    lw_atomic_init();
}

void lw_lock_destroy(void)
{
    lw_atomic_destroy();
}

