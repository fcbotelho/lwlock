#ifndef __LW_CYCLES_H__
#define __LW_CYCLES_H__

#include "lw_types.h"
#include <time.h>

static inline lw_uint64_t
rdtsc(void)
{
#if (defined(AMD64_ASM))  // support for AMD 64 bit assembly
    lw_uint32_t high, low;
    asm volatile("rdtsc":"=a"(low), "=d"(high));
    return (((lw_uint64_t)high << 32) | low);
#else  // !defined(AMD64_ASM)
    struct timeval tv;
    int ret = 0;

    ret = gettimeofday(&tv, NULL);
    lw_verify(ret == 0);

    return (1000ULL * 1000 * tv.tv_sec) + tv.tv_usec;
#endif
}

/*
 * subtract two tsc values: end - start
 * if end is 'smaller' than start, return 0
 * allow tsc counter to wrap around
 */
#define LW_TSC_DIFF(end, start) \
    ((end) - (start) <= (LW_MAX_UINT64 >> 1)) ? ((end) - (start)) : 0
#define LW_TSC_DIFF_NOW(start) LW_TSC_DIFF(rdtsc(), (start))

extern lw_uint32_t lw_cpu_khz;
void lw_cycles_init(void);



#endif
