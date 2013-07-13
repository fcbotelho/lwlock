#ifndef __LW_TYPES_H__
#define __LW_TYPES_H__

#define LW_IN const
#define LW_OUT
#define LW_INOUT

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
  typedef long lw_int64_t;

  /** \typedef unsigned long lw_uint64_t;
   *  \brief Unsigned 64-bit integer for a 64-bit achitecture.
   */
  typedef unsigned long lw_uint64_t;
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

#endif
