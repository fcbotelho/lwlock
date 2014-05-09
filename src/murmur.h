#ifndef __MURMUR_H__
#define __MURMUR_H__

#include "lw_types.h"

/*
 * Taken from http://floodyberry.com/noncryptohashzoo/Murmur3.html and adapted
 * for types of variables.
 */

#define rot(n, r)    ((n >> r) | (n << (32 - r)))

#define mmix3(h,k) { k *= m1; k = rot(k,r1); k *= m2; h *= 3; h ^= k; }

static inline lw_uint32_t
Murmur3(const lw_uint8_t *key, lw_uint32_t len, lw_uint32_t seed )
{
    const lw_uint32_t m1 = 0x0acffe3d, m2 = 0x0e4ef5f3, m3 = 0xa729a897;
    const lw_uint32_t r1 = 11, r2 = 18, r3 = 18;

    lw_uint32_t h = len + seed, k = 0;

    const lw_uint32_t *dwords = (const lw_uint32_t *)key;
    while( len >= 4 ) {
        k = *dwords++;
        mmix3(h,k);
        len -= 4;
    }

    const lw_uint8_t *tail = (const lw_uint8_t *)dwords;
    switch( len ) {
        case 3: k ^= tail[2] << 16;
        case 2: k ^= tail[1] << 8;
        case 1: k ^= tail[0];
            mmix3(h,k);
    }

    h ^= h >> r2;
    h *= m3;
    h ^= h >> r3;
    return h;
}

/* Version of Murmur3 above adapated specifically for hashing a pointer itself. */
static inline lw_uint32_t
Murmur3Ptr(const void *ptr, lw_uint32_t seed)
{
    const lw_uint32_t m1 = 0x0acffe3d, m2 = 0x0e4ef5f3, m3 = 0xa729a897;
    const lw_uint32_t r1 = 11, r2 = 18, r3 = 18;
    const lw_uint64_t key64 = LW_PTR_2_NUM(ptr, lw_uint64_t);

    lw_uint32_t h = 8 + seed, k = key64 & 0xFFFFFFFF;
    mmix3(h,k);
    k = key64 >> 32;
    mmix3(h, k);

    h ^= h >> r2;
    h *= m3;
    h ^= h >> r3;
    return h;
}

#endif /* __MURMUR_H__ */
