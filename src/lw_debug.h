/***
 * Developed originally at EMC Corporation, this library is released under the
 * MPL 2.0 license.  Please refer to the MPL-2.0 file in the repository for its
 * full description or to http://www.mozilla.org/MPL/2.0/ for the online version.
 *
 * Before contributing to the project one needs to sign the committer agreement
 * available in the "committerAgreement" directory.
 */

#ifdef LW_DEBUGP
#undef LW_DEBUGP
#endif

#ifdef __cplusplus
#include <cstdio>
#ifdef WIN32
#include <cstring>
#endif
#else
#include <stdio.h>
#ifdef WIN32
#include <string.h>
#endif
#endif

#ifndef __GNUC__
#ifndef __LW_DEBUG_H__
#define __LW_DEBUG_H__
#include <stdarg.h>
static void lw_debugprintf(const char *format, ...)
{
    va_list ap;
	char *f = NULL;
	const char *p="%s:%d ";
	size_t plen = strlen(p);
    va_start(ap, format);
	f = (char *)malloc(plen + strlen(format) + 1);
	if (!f) return;
	memcpy(f, p, plen);
	memcpy(f + plen, format, strlen(format) + 1);
    vfprintf(stderr, f, ap);
    va_end(ap);
	free(f);
}
static void lw_dummyprintf(const char *format, ...)
{}
#endif
#endif

#ifdef LW_DEBUG
#ifndef __GNUC__
#define LW_DEBUGP lw_debugprintf
#else
#define LW_DEBUGP(args...) do { fprintf(stderr, "%s: %s: %d ", __FILE__, __FUNCTION__, __LINE__); fprintf(stderr, ## args); } while(0)
#endif
#else
#ifndef __GNUC__
#define LW_DEBUGP lw_dummyprintf
#else
#define LW_DEBUGP(args...)
#endif
#endif // LW_DEBUG

#ifdef LW_DEBUG
#include <assert.h>
#define lw_assert(arg) assert(arg)
#else
#define lw_assert(arg) LW_UNUSED_PARAMETER(arg)
#endif

#include "lw_compiler.h"
#include <stdlib.h>
#define lw_verify(_x) \
    do { \
        if (lw_predict_likely(_x)) { \
            /* noop */ \
        } else { \
            fprintf(stderr, \
                    "%s: %s: %d !(%s)", \
                    __FILE__, \
                    __FUNCTION__, \
                    __LINE__, \
                    #_x); \
            abort(); \
        } \
    } while (0)
