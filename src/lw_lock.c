#include "lw_type.h"
#include <dd_atomic.h>
#include "lw_cycles.h"

void lw_lock_init(void)
{
    lw_atomic_init();
    lw_cycles_init();
}

void lw_lock_destroy(void)
{
    lw_atomic_destroy();
}

