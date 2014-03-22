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
