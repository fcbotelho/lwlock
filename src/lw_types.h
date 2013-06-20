#ifndef __LW_TYPES_H__
#define __LW_TYPES_H__

typedef char lw_int8_t;
typedef unsigned char lw_uint8_t;

typedef short lw_int16_t;
typedef unsigned short lw_uint16_t;

typedef int lw_int32_t;
typedef unsigned int lw_uint32_t;

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

#endif
