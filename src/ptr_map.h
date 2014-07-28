/***
 *
 * Independently contributed to the lwlock library. This file is also released under the
 * terms of MPL 2.0 license but you do not need to sign the committer agreement for
 * this file.
 */

#ifndef __PTR_MAP_H__
#define __PTR_MAP_H__

#include "lw_types.h"

#define PTR_MAP_NULL     (0)

/*
 * ptr_maps define routines to convert to and from "pointers" of different sizes. The most common
 * case is to map 64 bit pointers to 32 bit pointers (often index into a flat array) so the remaining
 * 32 bits of the poitner can be used to encode other information that has to be maintained atomically
 * with the pointer.
 */

typedef struct ptr64_to32_map_s ptr64_to32_map_t;

typedef lw_uint32_t (*ptr_64_to_32_func_t)(ptr64_to32_map_t *map, void *ptr);
typedef void *(*ptr_32_to_64_funct_t)(ptr64_to32_map_t *map, lw_uint32_t id32);

struct ptr64_to32_map_s {
    ptr_64_to_32_func_t     obj2id;
    ptr_32_to_64_funct_t    id2obj;
};

typedef struct {
    ptr64_to32_map_t    funcs;
    lw_uint8_t          *base;
    lw_uint32_t         num_elems;
    lw_uint64_t         elem_size;
} ptr6432_map_array_t;

void ptr6432_map_array_init(ptr6432_map_array_t *map,
                            lw_uint8_t *base,
                            lw_uint32_t num_elems,
                            lw_uint64_t elem_size);

#endif /* __PTR_MAP_H__ */
