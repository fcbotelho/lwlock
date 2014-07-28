/***
 *
 * Independently contributed to the lwlock library. This file is also released under the
 * terms of MPL 2.0 license but you do not need to sign the committer agreement for
 * this file.
 */


/*
 * There are no include guards in this file by design. It is to allow generation
 * of delegate state based structures in multiple files.
 *
 * To generate one, define the DELEGATE_OTHER_FIELDS, and DELEGATE_STATE_TYPE_NAME
 * macro and then include this file.
 */

#define DELEGATE_STATE_STRUCT                           \
typedef union {                                         \
    lw_uint64_t atomic64;                               \
    struct {                                            \
        lw_uint64_t   lock:1;                           \
        lw_uint64_t   incoming:1;                       \
        lw_uint64_t   pending:1;                        \
        lw_uint64_t   waiting:1;                        \
        DELEGATE_OTHER_FIELDS                           \
    } fields;                                           \
} DELEGATE_STATE_TYPE_NAME;                             \
                                                        \
LW_STATIC_ASSERT(sizeof(DELEGATE_STATE_TYPE_NAME) ==    \
                 sizeof(lw_uint64_t), DELEGATE_STATE_TYPE_NAME ## _needs_64_bit_atomic)

DELEGATE_STATE_STRUCT;

