#ifndef __LW_ATOMIC_H__
#define __LW_ATOMIC_H__

#include "lw_types.h"

#if (defined(AMD64_ASM))  // support for AMD 64 bit assembly

/**
 * Compare and Exchange (32 bit)
 *
 * @param var (i) Pointer to a 32 bit integer value to modify
 * @param old (i) Value expected to be in var
 * @param new (i) New value to be stored in var
 *
 * @description
 *
 * This interface provide the ability to atomically compare and exchange
 * a 32 bit integer value.  This routine returns the value contained in
 * the specified var at the time the routine is called.  If a race
 * occurred and var nolonger contains the value specified by old, no
 * change is made, otherwise the new value is stored.
 *
 * @return The value originally contained in var.
 */
static inline lw_uint32_t
lw_uint32_cmpxchg(volatile lw_uint32_t *var,
                  lw_uint32_t old,
                  lw_uint32_t new)
{
    lw_uint32_t prev;

    asm volatile("lock; cmpxchgl %1,%2"
                  : "=a"(prev)
                  : "q"(new),
                  "m"(*var),
                  "0"(old)
                  : "memory");

    return prev;
} 


/**
 * Compare and Exchange (64 bit)
 *
 * @param var (i) Pointer to a 64 bit integer value to modify
 * @param old (i) Value expected to be in var
 * @param new (i) New value to be stored in var
 *
 * @description
 *
 * This interface provide the ability to atomically compare and exchange
 * a 64 bit integer value.  This routine returns the value contained in
 * the specified var at the time the routine is called.  If a race
 * occurred and var nolonger contains the value specified by old, no
 * change is made, otherwise the new value is stored.
 *
 * @return The value originally contained in var.
 */
static inline lw_uint64_t
lw_uint64_cmpxchg(volatile lw_uint64_t *var,
                  lw_uint64_t old,
                  lw_uint64_t new)
{
    lw_uint64_t prev;

    asm volatile("lock; cmpxchgq %1,%2"
                  : "=a"(prev)
                  : "q"(new),
                  "m"(*var),
                  "0"(old)
                  : "memory");

    return prev;
}

/* Atomically add increment value to the int pointed to by var */
static inline void __attribute__ ((always_inline))
lw_uint32_lock_add(volatile lw_uint32_t *var, lw_uint32_t increment)
{    
    lw_assert(((uintptr_t)var) % sizeof(*var) == 0);
    asm volatile("lock; addl %0, %1"
                 : 
                 : "q"(increment),
                   "m"(*var)
                 : "memory");
}

/* Atomically add increment to var */
static inline void __attribute__ ((always_inline))
lw_uint64_lock_add(volatile lw_uint64_t *var, lw_uint64_t increment)
{    
    dd_assert(((uintptr_t)var) % sizeof(*var) == 0);
    asm volatile("lock; addq %0, %1"
                 : 
                 : "q"(increment),
                   "m"(*var)
                 : "memory");
}

/* Atomically add increment to var. Return old value */
static inline lw_uint32_t __attribute__ ((always_inline))
lw_uint32_lock_xadd(volatile lw_uint32_t *var, lw_uint32_t increment)
{
    dd_assert(((uintptr_t)var) % sizeof(*var) == 0);
    asm volatile("lock; xaddl %0, %1"
                 : "=q"(increment)
                 : "m"(*var),
                   "0"(increment)
                 : "memory");
    return increment; /* This is old value! */
}
 
/* Atomically add increment to var. Return old value */
static inline lw_uint64_t __attribute__ ((always_inline))
lw_uint64_lock_xadd(volatile lw_uint64_t *var, lw_uint64_t increment)
{
    dd_assert(((uintptr_t)var) % sizeof(*var) == 0);
    asm volatile("lock; xaddq %0, %1"
                 : "=q"(increment)
                 : "m"(*var),
                   "0"(increment)
                 : "memory");
    return increment;
}


#else  // !defined(AMD64_ASM)
/* Note that we ignore if X86_ASM is defined. We need to add code for
 * this case if we decide to optimize this explicitly
 */

extern lw_uint32_t lw_uint32_cmpxchg(volatile lw_uint32_t *var, lw_uint32_t old, lw_uint32_t new);
extern lw_uint64_t lw_uint64_cmpxchg(volatile lw_uint64_t *var, lw_uint64_t old, lw_uint64_t new);

static inline void __attribute__ ((always_inline))
lw_uint32_lock_add(volatile lw_uint32_t *var, lw_uint32_t increment)
{

    lw_uint32_t old, new;

    do {
        old = *var;
        new = (old + increment);
    } while (lw_uint32_cmpxchg(var, old, new) != old);
}

static inline lw_uint32_t __attribute__ ((always_inline))
lw_uint32_lock_xadd(volatile lw_uint32_t *var, lw_uint32_t increment)
{
    lw_uint32_t old, new;

    do {
        old = *var;
        new = (old + increment);
    } while (lw_uint32_cmpxchg(var, old, new) != old);

    return (old); /* Not new value! */
}

static inline void __attribute__ ((always_inline))
lw_uint64_lock_add(volatile lw_uint64_t *var, lw_uint64_t increment)
{

    lw_uint64_t old, new;

    do {
        old = *var;
        new = (old + increment);
    } while (lw_uint64_cmpxchg(var, old, new) != old);
}

static inline lw_uint64_t __attribute__ ((always_inline))
lw_uint64_lock_xadd(volatile lw_uint64_t *var, lw_uint64_t increment)
{
    lw_uint64_t old, new;

    do {
        old = *var;
        new = (old + increment);
    } while (lw_uint64_cmpxchg(var, old, new) != old);

    return (old); /* Not new value! */
}


#endif



/* Define other atomic sub/dec/inc using above *_add and *_xadd above */

#define lw_uint32_lock_sub(var, sub)    lw_uint32_lock_add(var, (lw_uint32_t)(-(sub)))
#define lw_uint32_lock_xsub(var, sub)   lw_uint32_lock_xadd(var, (lw_uint32_t)(-(sub)))
#define lw_uint64_lock_sub(var, sub)    lw_uint64_lock_add(var, (lw_uint64_t)(-(sub)))
#define lw_uint64_lock_xsub(var, sub)   lw_uint64_lock_xadd(var, (lw_uint64_t)(-(sub)))

#define lw_uint32_lock_inc(var)         lw_uint32_lock_add(var, 1U)
#define lw_uint32_lock_xinc(var)        lw_uint32_lock_xadd(var, 1U)
#define lw_uint64_lock_inc(var)         lw_uint64_lock_add(var, 1ULL)
#define lw_uint64_lock_xinc(var)        lw_uint64_lock_xadd(var, 1ULL)

#define lw_uint32_lock_dec(var)         lw_uint32_lock_sub(var, 1U)
#define lw_uint64_lock_dec(var)         lw_uint64_lock_sub(var, 1ULL)
#define lw_uint32_lock_xdec(var)        lw_uint32_lock_xsub(var, 1U)
#define lw_uint64_lock_xdec(var)        lw_uint64_lock_xsub(var, 1ULL)

/**
 * Set a 32bit atomic variable (no return value)
 *
 * @param atomic (i) the input parameter specifying a pointer to an
 *     atomic variable
 * @param val (i) the input parameter specifying the integer value
 *     to be stored in the atomic variable.
 *
 * @description
 *
 * Set an atomic variable to a specified value and return that value.
 *
 * @return none
 */
static inline void __attribute__ ((always_inline))
lw_atomic32_set(lw_atomic32_t *atomic,
                lw_uint32_t val)
{
    atomic->val = val;
}

/**
 * Read a 32bit atomic variable
 *
 * @param atomic (i) the input parameter specifying a pointer to an
 *     atomic variable
 *
 * @description
 *
 * Read an atomic variable and return the current value.
 *
 * @return Value contained in atomic variable
 */
static inline lw_uint32_t __attribute__ ((always_inline))
lw_atomic32_read(const lw_atomic32_t *atomic)
{
    return (atomic->val);
}

/**
 * Increment a 32bit atomic variable (no return value)
 *
 * @param atomic (i) the input parameter specifying a pointer to an
 *     atomic variable
 *
 * @description
 *
 * Increment the atomic variable without returning the result.
 *
 * @return none
 */
static inline void __attribute__ ((always_inline))
lw_atomic32_inc(lw_atomic32_t *atomic)
{
    lw_uint32_lock_inc(&atomic->val);
}

/**
 * Increment a 32bit atomic variable (with return value)
 *
 * @param atomic (i) the input parameter specifying a pointer to an
 *     atomic variable
 *
 * @description
 *
 * Increment the atomic variable and return the result.
 *
 * @return Value following increment
 */
static inline lw_uint32_t __attribute__ ((always_inline))
lw_atomic32_inc_with_ret(lw_atomic32_t *atomic)
{
    return (lw_uint32_lock_xinc(&atomic->val) + 1); // dd_uint32_lock_xinc returns old value
}

/**
 * Decrement a 32bit atomic variable (no return value)
 *
 * @param atomic (i) the input parameter specifying a pointer to an
 *     atomic variable
 *
 * @description
 *
 * Decrement the atomic variable and return the result.
 *
 * @return none
 */
static inline void __attribute__ ((always_inline))
lw_atomic32_dec(lw_atomic32_t *atomic)
{
    lw_uint32_lock_dec(&atomic->val);
}

/**
 * Decrement a 32bit atomic variable (with return value)
 *
 * @param atomic (i) the input parameter specifying a pointer to an
 *     atomic variable
 *
 * @description
 *
 * Decrement the atomic variable and return the result.
 *
 * @return Value following decrement
 */
static inline lw_uint32_t __attribute__ ((always_inline))
lw_atomic32_dec_with_ret(lw_atomic32_t *atomic)
{
    return lw_uint32_lock_xdec(&atomic->val) - 1;
}

/**
 * Add to a 32bit atomic variable (no return value)
 *
 * @param atomic (i) the input parameter specifying a pointer to an
 *     atomic variable
 * @param i (i) value to be added to the atomic variable
 *
 * @description
 *
 * Add value i to the current value in the atomic variable and return
 * the result.
 *
 * @return none
 */
static inline void __attribute__ ((always_inline))
lw_atomic32_add(lw_atomic32_t *atomic,
                lw_uint32_t i)
{
    lw_uint32_lock_add(&atomic->val, i);
}

/**
 * Add to a 32bit atomic variable (with return value)
 *
 * @param atomic (i) the input parameter specifying a pointer to an
 *     atomic variable
 * @param i (i) value to be added to the atomic variable
 *
 * @description
 *
 * Add value i to the current value in the atomic variable and return
 * the result.
 *
 * @return Value following addition
 */
static inline lw_uint32_t __attribute__ ((always_inline))
lw_atomic32_add_with_ret(lw_atomic32_t *atomic,
                         lw_uint32_t i)
{
    return lw_uint32_lock_xadd(&atomic->val, i) + i;
}

/**
 * Subtract from a 32bit atomic variable (no return value)
 *
 * @param atomic (i) the input parameter specifying a pointer to an
 *     atomic variable
 * @param i (i) value to be subtracted to the atomic variable
 *
 * @description
 *
 * Subtract value i from the current value in the atomic variable and
 * return the result.
 *
 * @return none
 */
static inline void __attribute__ ((always_inline))
lw_atomic32_sub(lw_atomic32_t *atomic, lw_uint32_t i)
{
    lw_uint32_lock_sub(&atomic->val, i);
}

/**
 * Subtract from a 32bit atomic variable (with return value)
 *
 * @param atomic (i) the input parameter specifying a pointer to an
 *     atomic variable
 * @param i (i) value to be subtracted to the atomic variable
 *
 * @description
 *
 * Subtract value i from the current value in the atomic variable and
 * return the result.
 *
 * @return Value following subtraction
 */
static inline lw_uint32_t __attribute__ ((always_inline))
lw_atomic32_sub_with_ret(lw_atomic32_t *atomic, lw_uint32_t i)
{
    return lw_uint32_lock_xsub(&atomic->val, i) - i;
}


/**
 * Set a 64bit atomic variable (no return value)
 *
 * @param atomic (i) the input parameter specifying a pointer to an
 *     atomic variable
 * @param val (i) the input parameter specifying the integer value
 *     to be stored in the atomic variable.
 *
 * @description
 *
 * Set an atomic variable to a specified value and return that value.
 *
 * @return none
 */
static inline void __attribute__ ((always_inline))
lw_atomic64_set(lw_atomic64_t *atomic, lw_uint64_t val)
{
    atomic->val = val;
}

/**
 * Read a 64bit atomic variable
 *
 * @param atomic (i) the input parameter specifying a pointer to an
 *     atomic variable
 *
 * @description
 *
 * Read an atomic variable and return the current value.
 *
 * @return Value contained in atomic variable
 */
static inline lw_uint64_t __attribute__ ((always_inline))
lw_atomic64_read(const lw_atomic64_t *atomic)
{
    return (atomic->val);
}

/**
 * Increment a 64bit atomic variable (no return value)
 *
 * @param atomic (i) the input parameter specifying a pointer to an
 *     atomic variable
 *
 * @description
 *
 * Increment the atomic variable without returning the result.
 *
 * @return none
 */
static inline void __attribute__ ((always_inline))
lw_atomic64_inc(lw_atomic64_t *atomic)
{
    lw_uint64_lock_inc(&atomic->val);
}

/**
 * Increment a 64bit atomic variable (with return value)
 *
 * @param atomic (i) the input parameter specifying a pointer to an
 *     atomic variable
 *
 * @description
 *
 * Increment the atomic variable and return the result.
 *
 * @return Value following increment
 */
static inline lw_uint64_t __attribute__ ((always_inline))
lw_atomic64_inc_with_ret(lw_atomic64_t *atomic)
{
    return lw_uint64_lock_xinc(&atomic->val) + 1;
}

/**
 * Decrement a 64bit atomic variable (no return value)
 *
 * @param atomic (i) the input parameter specifying a pointer to an
 *     atomic variable
 *
 * @description
 *
 * Decrement the atomic variable and return the result.
 *
 * @return none
 */
static inline void __attribute__ ((always_inline))
lw_atomic64_dec(lw_atomic64_t *atomic)
{
    lw_uint64_lock_dec(&atomic->val);
}

/**
 * Decrement a 64bit atomic variable (with return value)
 *
 * @param atomic (i) the input parameter specifying a pointer to an
 *     atomic variable
 *
 * @description
 *
 * Decrement the atomic variable and return the result.
 *
 * @return Value following decrement
 */
static inline lw_uint64_t __attribute__ ((always_inline))
lw_atomic64_dec_with_ret(lw_atomic64_t *atomic)
{
    return lw_uint64_lock_xdec(&atomic->val) - 1;
}

/**
 * Add to a 64bit atomic variable (no return value)
 *
 * @param atomic (i) the input parameter specifying a pointer to an
 *     atomic variable
 * @param i (i) value to be added to the atomic variable
 *
 * @description
 *
 * Add value i to the current value in the atomic variable and return
 * the result.
 *
 * @return none
 */
static inline void __attribute__ ((always_inline))
lw_atomic64_add(lw_atomic64_t *atomic, lw_uint64_t i)
{
    lw_uint64_lock_add(&atomic->val, i);
}

/**
 * Add to a 64bit atomic variable (with return value)
 *
 * @param atomic (i) the input parameter specifying a pointer to an
 *     atomic variable
 * @param i (i) value to be added to the atomic variable
 *
 * @description
 *
 * Add value i to the current value in the atomic variable and return
 * the result.
 *
 * @return Value following addition
 */
static inline lw_uint64_t __attribute__ ((always_inline))
lw_atomic64_alw_with_ret(lw_atomic64_t *atomic, lw_uint64_t i)
{
    return lw_uint64_lock_xadd(&atomic->val, i) + i;
}

/**
 * Subtract from a 64bit atomic variable (no return value)
 *
 * @param atomic (i) the input parameter specifying a pointer to an
 *     atomic variable
 * @param i (i) value to be subtracted to the atomic variable
 *
 * @description
 *
 * Subtract value i from the current value in the atomic variable and
 * return the result.
 *
 * @return none
 */
static inline void __attribute__ ((always_inline))
lw_atomic64_sub(lw_atomic64_t *atomic, lw_uint64_t i)
{
    lw_uint64_lock_sub(&atomic->val, i);
}

/**
 * Subtract from a 64bit atomic variable (with return value)
 *
 * @param atomic (i) the input parameter specifying a pointer to an
 *     atomic variable
 * @param i (i) value to be subtracted to the atomic variable
 *
 * @description
 *
 * Subtract value i from the current value in the atomic variable and
 * return the result.
 *
 * @return Value following subtraction
 */
static inline lw_uint64_t __attribute__ ((always_inline))
lw_atomic64_sub_with_ret(lw_atomic64_t *atomic, lw_uint64_t i)
{
    return lw_uint64_lock_xsub(&atomic->val, i) - i;
}

