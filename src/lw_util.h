/***
 * Developed originally at EMC Corporation, this library is released under the
 * MPL 2.0 license.  Please refer to the MPL-2.0 file in the repository for its
 * full description or to http://www.mozilla.org/MPL/2.0/ for the online version.
 *
 * Before contributing to the project one needs to sign the committer agreement
 * available in the "committerAgreement" directory.
 */

#ifndef __LW_UTIL_H__
#define __LW_UTIL_H__
#include <stdarg.h>

/**
 *  Emit a print string and store it in the specified buffer.
 *
 *  @param buf (i) Pointer to buffer to store string.
 *  @param len (i) Size of string buffer.
 *  @param pos (i) Pointer to string position value
 *                (Should be initialized to 0 on first call)
 *  @param fmt (i) the input parameter specifying the printf-like
 *                 format specification.
 *  @param var-args (i) optional parameters corresponding to the
 *                      specified format.
 *
 *
 *  @description
 *
 *  This routine is similar to snprintf(), but oriented towards emiting
 *  a sequence of strings and storing them one after another in a single
 *  buffer.  This routine tracks the position in the buffer and returns
 *  -1 if the buffer length is exceeded.
 */
static inline int
lw_printbuf(char *buf, size_t len, size_t *pos, const char *fmt, ...)
{
    va_list args;
    int ret;

    lw_verify(*pos < len);

    va_start(args, fmt);
    ret = vsnprintf(&buf[*pos], (len - *pos), fmt, args);
    va_end(args);

    if (ret < 0 || (size_t)ret >= (len - *pos)) {
        *pos += (len - *pos - 1);
        return -1;
    } else {
        *pos += ret;
        return 0;
    }
}

static inline void *
lw_cast_no_const(const void *ptr)
{
    union {
        const void *const_ptr;
        void *ptr;
    } tptr;

    tptr.const_ptr = ptr;
    return tptr.ptr;
}

#endif
