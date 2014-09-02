/***
 * Developed originally at EMC Corporation, this library is released under the
 * MPL 2.0 license.  Please refer to the MPL-2.0 file in the repository for its
 * full description or to http://www.mozilla.org/MPL/2.0/ for the online version.
 *
 * Before contributing to the project one needs to sign the committer agreement
 * available in the "committerAgreement" directory.
 */

#ifndef __LW_TYPES_H__
#define __LW_TYPES_H__

#include <stdint.h>

#define ALIGNED_PACKED(_a)      __attribute__((aligned(_a), packed))
#define ALIGNED(_a)             __attribute__((aligned(_a)))
#define PACKED                  __attribute__((packed))
#define INLINE                  inline
#define ALWAYS_INLINE           __attribute__ ((always_inline))

#define LW_IN const
#define LW_OUT
#define LW_INOUT

#ifndef FALSE
#define FALSE   (0)
#endif
#ifndef TRUE
#define TRUE    (1)
#endif

typedef char lw_int8_t;
typedef unsigned char lw_uint8_t;

typedef short lw_int16_t;
typedef unsigned short lw_uint16_t;

typedef int lw_int32_t;
typedef unsigned int lw_uint32_t;

typedef lw_uint32_t lw_bool_t;

#if defined(__ia64) || defined(__x86_64__)
  /** \typedef long lw_int64_t;
   *  \brief 64-bit integer for a 64-bit achitecture.
   */
  typedef long long lw_int64_t;

  /** \typedef unsigned long lw_uint64_t;
   *  \brief Unsigned 64-bit integer for a 64-bit achitecture.
   */
  typedef unsigned long long lw_uint64_t;
#else
  /** \typedef long long lw_int64_t;
   *  \brief 64-bit integer for a 32-bit achitecture.
   */
  typedef long long lw_int64_t;

  /** \typedef unsigned long long lw_uint64_t;
   *  \brief Unsigned 64-bit integer for a 32-bit achitecture.
   */
  typedef unsigned long long lw_uint64_t;
#endif

typedef struct {
    volatile lw_uint32_t  val;
} lw_atomic32_t;


typedef struct {
    volatile lw_uint64_t  val;
} lw_atomic64_t;

/* A generic callback function signature type. */
typedef void (*callback_func_t)(void *arg);

#define LW_MAX_UINT16 ((lw_uint16_t)~0)
#define LW_MAX_UINT32 ((lw_uint32_t)~0)
#define LW_MAX_UINT64 ((lw_uint64_t)~0)

/*
 * Most OS do not allow a massive number of threads. So 16 bits suffice
 * for now.
 */
typedef lw_uint16_t lw_waiter_id_t;

#define LW_WAITER_ID_MAX LW_MAX_UINT16

/*
 *   Allow intentionally unused parameters to compile.
 */
#define LW_UNUSED_PARAMETER(x) ((void) (x))

/*
 *   Allow intentionally ignored function return value to compile.
 */
#define LW_IGNORE_RETURN_VALUE(x) ((void) (x))

/*
 * Conversion between pointers and numbers.
 *
 * Works for both 32 and 64 bit pointers.
 */
#define LW_PTR_2_NUM(__ptr, __type) ((typeof(__type))((uintptr_t)(__ptr)))
#define LW_NUM_2_PTR(__num, __type) ((typeof(__type) *)((uintptr_t)(__num)))

#define LW_OFFSET_OF(_type, _field)         \
    LW_PTR_2_NUM((&(((__typeof__(_type) *)0)->_field)), lw_uint64_t)

#define LW_FIELD_2_OBJ(_ptr, _type, _field)  \
    ((__typeof__(_type) *)((uintptr_t)(_ptr) - LW_OFFSET_OF(_type, _field)))

#define LW_FIELD_2_OBJ_NULL_SAFE(_ptr, _type, _field)   \
    ((_ptr) == NULL ? NULL : LW_FIELD_2_OBJ(_ptr, _type, _field))

#define LW_IS_POW2(_x)          (((_x) & ((_x) - 1)) == 0)
#endif
