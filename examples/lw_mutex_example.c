#include "lw_lock.h"
int main(int argc, char **argv)
{
    lw_mutex_t lw_mutex;
    lw_mutex_init(&lw_mutex);
    lw_mutex_destroy(&lw_mutex);
    return 0;
}
