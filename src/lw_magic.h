/***
 * Developed originally at EMC Corporation, this library is released under the
 * MPL 2.0 license.  Please refer to the MPL-2.0 file in the repository for its
 * full description or to http://www.mozilla.org/MPL/2.0/ for the online version.
 *
 * Before contributing to the project one needs to sign the committer agreement
 * available in the "committerAgreement" directory.
 */

#ifndef __LW_MAGIC_H__
#define __LW_MAGIC_H__

#include "lw_types.h"

/*
 * This include file contains definitions to construct in-memory
 * "magic numbers".  These magic numbers provide hints about
 * the contents of memory when other context is unavailable
 * (e.g., when scouring a core file).
 *
 * We strive to guarantee that magic numbers are unique.
 *
 * When executed from the top of a source tree, the following command
 * will find all instances of previously defined magic numbers:
 *
 *    find . -name '*.[ch]' | xargs fgrep 'LW_MAGIC('
 */

#define LW_MAGIC_BASE  0x5FAB    /* it is a positive number */

/*
 * Verify that the provided magic number:
 *    1) Is a hex value
 *    2) Fits within 16 bits
 *
 * We verify both (1) & (2) by appending sufficient hex digits to the
 * value to cause 64-bit overflow if the original value does not fit
 * in 16 bits.  We then right-shift the value 48 bits to obtain the
 * desired 16-bit value.
 */
#define _LW_VERIFY_HEX16(_n) ((lw_uint32_t) ((_n ## ffffffffffffULL) >> 48))

/*
 * The result of LW_MAGIC() is a 32-bit value whose upper 16 bits are
 * LW_MAGIC_BASE. These values are appropriate for use in enumerations.
 */
#define LW_MAGIC(_magic_number)   ((LW_MAGIC_BASE << 16) | _LW_VERIFY_HEX16(_magic_number))

/*
 * Define a type that can be used in structures.
 */
typedef lw_uint32_t lw_magic_t;

#endif /* __LW_MAGIC_H__ */
