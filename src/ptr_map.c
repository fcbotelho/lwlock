/***
 *
 * Independently contributed to the lwlock library. This file is also released under the
 * terms of MPL 2.0 license but you do not need to sign the committer agreement for
 * this file.
 */

#include "lw_debug.h"
#include "ptr_map.h"

#define MAP_IDX_OFFSET      (PTR_MAP_NULL + 1)

static lw_uint32_t
obj_to_id32(ptr64_to32_map_t *ptr6432, void *obj)
{
    ptr6432_map_array_t *map = (ptr6432_map_array_t *)ptr6432;

    lw_uint64_t offset = (lw_uint8_t *)obj - map->base;
    lw_uint32_t idx = offset / map->elem_size;

    lw_assert(offset % map->elem_size == 0);
    lw_assert(idx < map->num_elems);
    return idx + MAP_IDX_OFFSET;
}

static void *
id32_to_obj(ptr64_to32_map_t *ptr6432, lw_uint32_t idx)
{
    ptr6432_map_array_t *map = (ptr6432_map_array_t *)ptr6432;
    if (idx == PTR_MAP_NULL) {
        return NULL;
    }

    lw_assert(idx >= MAP_IDX_OFFSET && idx < map->num_elems + MAP_IDX_OFFSET);
    return map->base + (idx - MAP_IDX_OFFSET) * map->elem_size;
}

void
ptr6432_map_array_init(ptr6432_map_array_t *map,
                       lw_uint8_t *base,
                       lw_uint32_t num_elems,
                       lw_uint64_t elem_size)
{
    map->base = base;
    map->num_elems = num_elems;
    map->elem_size = elem_size;
    map->funcs.obj2id = obj_to_id32;
    map->funcs.id2obj = id32_to_obj;
}
