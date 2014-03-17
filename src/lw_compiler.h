#ifndef LW_COMPILER
#define LW_COMPILER

#if defined(__GNUC__) && __GNUC__ >= 3
/* gcc >= 3 */
#define lw_predict_likely(x) __builtin_expect((x),1)
#define lw_predict_unlikely(x) __builtin_expect((x),0)
#else
/* not gcc */
#define lw_predict_likely(x) (x)
#define lw_predict_unlikely(x) (x)
#endif

#endif
