#include "lw_cycles.h"
#include <unistd.h>

lw_uint32_t lw_cpu_khz;

/* This initializes lw_cpu_khz to convert cycles to secs
 */
void
lw_cycles_init(void)
{
    if (lw_cpu_khz == 0) {

        lw_uint64_t t_start, t_stop;
        struct timeval tv_start;
        struct timeval tv_stop;
        int ret = 0;
        lw_uint64_t usec_elapsed = 0;

        ret = gettimeofday(&tv_start, NULL);
        lw_verify(ret == 0);

        t_start = lw_rdtsc();

        lw_sleep(1);

        ret = gettimeofday(&tv_stop, NULL);
        lw_verify(ret == 0);

        t_stop = lw_rdtsc();

        usec_elapsed = (1000ULL * 1000 * tv_stop.tv_sec) + tv_stop.tv_usec;
        usec_elapsed -= (1000ULL * 1000 * tv_start.tv_sec) + tv_start.tv_usec;

        /*
         * Avoid divide by 0. 
         * We tried to sleep for 1 sec above, so use 1000000L if we have to 
         * hard-code a value.
         */
        if (usec_elapsed == 0) {
            usec_elapsed = 1000000L;
        }

        lw_cpu_khz = (t_stop - t_start) * 1000 / usec_elapsed;
    }
}
