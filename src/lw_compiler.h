/***
 * Developed originally at EMC Corporation, this library is released under the
 * MPL 2.0 license.  Please refer to the MPL-2.0 file in the repository for its
 * full description or to http://www.mozilla.org/MPL/2.0/ for the online version.
 *
 * Before contributing to the project one needs to sign the committer agreement
 * available in the "committerAgreement" directory.
 */

#ifndef LW_COMPILER
#define LW_COMPILER

#if defined(__GNUC__) && __GNUC__ >= 3
/* gcc >= 3 */
#define lw_predict_likely(_x) __builtin_expect(_x, 1)
#define lw_predict_unlikely(_x) __builtin_expect(_x, 0)
#else
/* not gcc */
#define lw_predict_likely(_x) _x
#define lw_predict_unlikely(_x) _x
#endif

#endif
