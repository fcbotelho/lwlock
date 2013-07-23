#ifndef __LW_ATOMIC_H__
#define __LW_ATOMIC_H__

#include "lw_types.h"

#if (defined(AMD64_ASM))  // support for AMD 64 bit assembly

/*
 * There is no need for lw_atomic_init and lw_atomic_destroy when
 * AMD64_ASM is defined
 */
#define lw_atomic_init
#define lw_atomic_destroy

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
lw_uint32_cmpxchg(LW_INOUT volatile lw_uint32_t *var,
                  LW_IN lw_uint32_t old,
                  LW_IN lw_uint32_t new)
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

/**
 * Atomic increment (32 bit)
 * 
 * @param var (i/o) pointer to an integer that will be 
 *            atomically incremented.
 * @param increment (i) value to be incremented.
 *
 * @description
 *
 * Atomically add increment value to the int pointed to by var 
 */
static inline void __attribute__ ((always_inline))
lw_uint32_lock_add(LW_INOUT volatile lw_uint32_t *var, 
                   LW_IN lw_uint32_t increment)
{    
    lw_assert(((uintptr_t)var) % sizeof(*var) == 0);
    asm volatile("lock; addl %0, %1"
                 : 
                 : "q"(increment),
                   "m"(*var)
                 : "memory");
}

/**
 * Atomic increment (64 bit)
 * 
 * @param var (i/o) pointer to an integer that will be 
 *            atomically incremented.
 * @param increment (i) value to be incremented.
 *
 * @description
 *
 * Atomically add increment value to the int pointed to by var 
 */
static inline void __attribute__ ((always_inline))
lw_uint64_lock_add(LW_INOUT volatile lw_uint64_t *var, 
                   LW_IN lw_uint64_t increment)
{    
    lw_assert(((uintptr_t)var) % sizeof(*var) == 0);
    asm volatile("lock; addq %0, %1"
                 : 
                 : "q"(increment),
                   "m"(*var)
                 : "memory");
}

/**
 * Atomic increment with the return of the old value (32 bit)
 * 
 * @param var (i/o) pointer to an integer that will be 
 *            atomically incremented.
 * @param increment (i) value to be incremented.
 *
 * @description
 *
 * Atomically add increment value to the int pointed to by var 
 * 
 * @return The value originally contained in var.
 */
static inline lw_uint32_t __attribute__ ((always_inline))
lw_uint32_lock_xadd(LW_INOUT volatile lw_uint32_t *var, 
                    LW_IN lw_uint32_t increment)
{
    dd_assert(((uintptr_t)var) % sizeof(*var) == 0);
    asm volatile("lock; xaddl %0, %1"
                 : "=q"(increment)
                 : "m"(*var),
                   "0"(increment)
                 : "memory");
    return increment; /* This is old value! */
}
 
/**
 * Atomic increment with the return of the old value (64 bit)
 * 
 * @param var (i/o) pointer to an integer that will be 
 *            atomically incremented.
 * @param increment (i) value to be incremented.
 *
 * @description
 *
 * Atomically add increment value to the int pointed to by var 
 * 
 * @return The value originally contained in var.
 */
static inline lw_uint64_t __attribute__ ((always_inline))
lw_uint64_lock_xadd(volatile lw_uint64_t *var, lw_uint64_t increment)
{
    dd_assert(((uintptr_t)var) % sizeof(*var) == 0);
    asm volatile("lock; xaddq %0, %1"
                 : "=q"(increment)
                 : "m"(*var),
                   "0"(increment)
                 : "memory");
    return increment; /* This is old value! */
}


#else  // !defined(AMD64_ASM)
/* Note that we ignore if X86_ASM is defined. We need to add code for
 * this case if we decide to optimize this explicitly
 */


/*
 * The lw_atomic_init and lw_atomic_destroy APIs are only needed 
 * when AMD64_ASM is not defined. They are used to initialize/destroy
 * a pthread mutex used to implement the other APIs in case the 
 * platform does not support atomic operations natively.
 */
extern void
lw_atomic_init(void);

extern void
lw_atomic_destroy(void);


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
extern lw_uint32_t 
lw_uint32_cmpxchg(LW_INOUT volatile lw_uint32_t *var, 
                  LW_IN lw_uint32_t old, 
                  LW_IN lw_uint32_t new);

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
extern lw_uint64_t 
lw_uint64_cmpxchg(LW_INOUT volatile lw_uint64_t *var, 
                  LW_IN lw_uint64_t old, 
                  LW_IN lw_uint64_t new);


/**
 * Atomic increment (32 bit)
 * 
 * @param var (i/o) pointer to an integer that will be 
 *            atomically incremented.
 * @param increment (i) value to be incremented.
 *
 * @description
 *
 * Atomically add increment value to the int pointed to by var 
 */
static inline void __attribute__ ((always_inline))
lw_uint32_lock_add(LW_INOUT volatile lw_uint32_t *var, 
                   LW_IN lw_uint32_t increment)
{

    lw_uint32_t old, new;

    do {
        old = *var;
        new = (old + increment);
    } while (lw_uint32_cmpxchg(var, old, new) != old);
}


/**
 * Atomic increment with the return of the old value (32 bit)
 * 
 * @param var (i/o) pointer to an integer that will be 
 *            atomically incremented.
 * @param increment (i) value to be incremented.
 *
 * @description
 *
 * Atomically add increment value to the int pointed to by var 
 * 
 * @return The value originally contained in var.
 */
static inline lw_uint32_t __attribute__ ((always_inline))
lw_uint32_lock_xadd(LW_INOUT volatile lw_uint32_t *var, 
                    LW_IN lw_uint32_t increment)
{
    lw_uint32_t old, new;

    do {
        old = *var;
        new = (old + increment);
    } while (lw_uint32_cmpxchg(var, old, new) != old);

    return (old); /* the old value! */
}

/**
 * Atomic increment (64 bit)
 * 
 * @param var (i/o) pointer to an integer that will be 
 *            atomically incremented.
 * @param increment (i) value to be incremented.
 *
 * @description
 *
 * Atomically add increment value to the int pointed to by var 
 */
static inline void __attribute__ ((always_inline))
lw_uint64_lock_add(LW_INOUT volatile lw_uint64_t *var, 
                   LW_IN lw_uint64_t increment)
{

    lw_uint64_t old, new;

    do {
        old = *var;
        new = (old + increment);
    } while (lw_uint64_cmpxchg(var, old, new) != old);
}

/**
 * Atomic increment with the return of the old value (64 bit)
 * 
 * @param var (i/o) pointer to an integer that will be 
 *            atomically incremented.
 * @param increment (i) value to be incremented.
 *
 * @description
 *
 * Atomically add increment value to the int pointed to by var 
 * 
 * @return The value originally contained in var.
 */
static inline lw_uint64_t __attribute__ ((always_inline))
lw_uint64_lock_xadd(LW_INOUT volatile lw_uint64_t *var, 
                    LW_IN lw_uint64_t increment)
{
    lw_uint64_t old, new;

    do {
        old = *var;
        new = (old + increment);
    } while (lw_uint64_cmpxchg(var, old, new) != old);

    return (old); /* the old value! */
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
 * set an atomic variable (32-bit)
 *
 * @param atomic (i) the input parameter specifying a pointer to an
 *                   atomic variable
 * @param val (i) the input parameter specifying the integer value
 *                to be stored in the atomic variable.
 *
 * @description
 *
 * set an atomic variable to a specified value.
 *
 * @return none
 */
static inline void __attribute__ ((always_inline))
lw_atomic32_set(LW_INOUT lw_atomic32_t *atomic,
                LW_IN lw_uint32_t val)
{
    atomic->val = val;
}

/**
 * Read the value stored in an atomic variable (32-bit)
 *
 * @param atomic (i) the input parameter specifying a pointer to an
 *                   atomic variable
 *
 * @description
 *
 * Read the value stored in an atomic variable and return it.
 *
 * @return Value contained in the input atomic variable
 */
static inline lw_uint32_t __attribute__ ((always_inline))
lw_atomic32_read(LW_IN lw_atomic32_t *atomic)
{
    return (atomic->val);
}

/**
 * Increment an atomic variable (32-bit)
 *
 * @param atomic (i) the input parameter specifying a pointer to an
 *                   atomic variable
 *
 * @description
 *
 * Increment the atomic variable.
 *
 * @return none
 */
static inline void __attribute__ ((always_inline))
lw_atomic32_inc(LW_INOUT lw_atomic32_t *atomic)
{
    lw_uint32_lock_inc(&atomic->val);
}

/**
 * Increment an atomic variable and return the value (32-bit)
 *
 * @param atomic (i) the input parameter specifying a pointer to an
 *                   atomic variable
 *
 * @description
 *
 * Increment the atomic variable and return the result.
 *
 * @return Value following increment
 */
static inline lw_uint32_t __attribute__ ((always_inline))
lw_atomic32_inc_with_ret(LW_INOUT lw_atomic32_t *atomic)
{
    // dd_uint32_lock_xinc returns old value
    return (lw_uint32_lock_xinc(&atomic->val) + 1); 
}

/**
 * Decrement an atomic variable (32-bit)
 *
 * @param atomic (i) the input parameter specifying a pointer to an
 *                   atomic variable
 *
 * @description
 *
 * Decrement the atomic variable.
 *
 * @return none
 */
static inline void __attribute__ ((always_inline))
lw_atomic32_dec(LW_INOUT lw_atomic32_t *atomic)
{
    lw_uint32_lock_dec(&atomic->val);
}

/**
 * Decrement an atomic variable (32-bit)
 *
 * @param atomic (i) the input parameter specifying a pointer to an
 *                   atomic variable
 *
 * @description
 *
 * Decrement the atomic variable.
 *
 * @return Value following decrement
 */
static inline lw_uint32_t __attribute__ ((always_inline))
lw_atomic32_dec_with_ret(LW_INOUT lw_atomic32_t *atomic)
{
    return lw_uint32_lock_xdec(&atomic->val) - 1;
}

/**
 * Add to an atomic variable (32-bit)
 *
 * @param atomic (i) the input parameter specifying a pointer to an
 *                   atomic variable
 * @param i (i) value to be added to the atomic variable
 *
 * @description
 *
 * Add value i to the current value in the atomic variable.
 *
 * @return none
 */
static inline void __attribute__ ((always_inline))
lw_atomic32_add(LW_INOUT lw_atomic32_t *atomic,
                LW_IN lw_uint32_t i)
{
    lw_uint32_lock_add(&atomic->val, i);
}

/**
 * Add to an atomic variable (32-bit)
 *
 * @param atomic (i) the input parameter specifying a pointer to an
 *                   atomic variable
 * @param i (i) value to be added to the atomic variable
 *
 * @description
 *
 * Add value i to the current value in the atomic variable.
 *
 * @return Value following addition
 */
static inline lw_uint32_t __attribute__ ((always_inline))
lw_atomic32_add_with_ret(LW_INOUT lw_atomic32_t *atomic,
                         LW_IN lw_uint32_t i)
{
    return lw_uint32_lock_xadd(&atomic->val, i) + i;
}

/**
 * Subtract from an atomic variable (32-bit)
 *
 * @param atomic (i) the input parameter specifying a pointer to an
 *                   atomic variable
 * @param i (i) value to be subtracted to the atomic variable
 *
 * @description
 *
 * Subtract value i from the current value in the atomic variable.
 *
 * @return none
 */
static inline void __attribute__ ((always_inline))
lw_atomic32_sub(LW_INOUT lw_atomic32_t *atomic, 
                LW_IN lw_uint32_t i)
{
    lw_uint32_lock_sub(&atomic->val, i);
}

/**
 * Subtract from an atomic variable (32-bit)
 *
 * @param atomic (i) the input parameter specifying a pointer to an
 *                   atomic variable
 * @param i (i) value to be subtracted to the atomic variable
 *
 * @description
 *
 * Subtract value i from the current value in the atomic variable.
 *
 * @return Value following subtraction
 */
static inline lw_uint32_t __attribute__ ((always_inline))
lw_atomic32_sub_with_ret(LW_INOUT lw_atomic32_t *atomic, 
                         LW_IN lw_uint32_t i)
{
    return lw_uint32_lock_xsub(&atomic->val, i) - i;
}


/**
 * set an atomic variable (64-bit)
 *
 * @param atomic (i) the input parameter specifying a pointer to an
 *                   atomic variable
 * @param val (i) the input parameter specifying the integer value
 *                to be stored in the atomic variable.
 *
 * @description
 *
 * set an atomic variable to a specified value.
 *
 * @return none
 */
static inline void __attribute__ ((always_inline))
lw_atomic64_set(LW_INOUT lw_atomic64_t *atomic, 
                LW_IN lw_uint64_t val)
{
    atomic->val = val;
}

 /**
 * Read the value stored in an atomic variable (64-bit)
 *
 * @param atomic (i) the input parameter specifying a pointer to an
 *                   atomic variable
 *
 * @description
 *
 * Read the value stored in an atomic variable and return it.
 *
 * @return Value contained in the input atomic variable
 */
static inline lw_uint64_t __attribute__ ((always_inline))
lw_atomic64_read(LW_IN lw_atomic64_t *atomic)
{
    return (atomic->val);
}

/**
 * Increment an atomic variable (64-bit)
 *
 * @param atomic (i) the input parameter specifying a pointer to an
 *                   atomic variable
 *
 * @description
 *
 * Increment the atomic variable.
 *
 * @return none
 */
static inline void __attribute__ ((always_inline))
lw_atomic64_inc(LW_INOUT lw_atomic64_t *atomic)
{
    lw_uint64_lock_inc(&atomic->val);
}

 /**
 * Increment an atomic variable and return the value (64-bit)
 *
 * @param atomic (i) the input parameter specifying a pointer to an
 *                   atomic variable
 *
 * @description
 *
 * Increment the atomic variable and return the result.
 *
 * @return Value following increment
 */
static inline lw_uint64_t __attribute__ ((always_inline))
lw_atomic64_inc_with_ret(LW_INOUT lw_atomic64_t *atomic)
{
    return lw_uint64_lock_xinc(&atomic->val) + 1;
}

/**
 * Decrement an atomic variable (64-bit)
 *
 * @param atomic (i) the input parameter specifying a pointer to an
 *                   atomic variable
 *
 * @description
 *
 * Decrement the atomic variable.
 *
 * @return none
 */
static inline void __attribute__ ((always_inline))
lw_atomic64_dec(LW_INOUT lw_atomic64_t *atomic)
{
    lw_uint64_lock_dec(&atomic->val);
}

/**
 * Decrement an atomic variable (64-bit)
 *
 * @param atomic (i) the input parameter specifying a pointer to an
 *                   atomic variable
 *
 * @description
 *
 * Decrement the atomic variable.
 *
 * @return Value following decrement
 */
static inline lw_uint64_t __attribute__ ((always_inline))
lw_atomic64_dec_with_ret(LW_INOUT lw_atomic64_t *atomic)
{
    return lw_uint64_lock_xdec(&atomic->val) - 1;
}

/**
 * Add to an atomic variable (64-bit)
 *
 * @param atomic (i) the input parameter specifying a pointer to an
 *                   atomic variable
 * @param i (i) value to be added to the atomic variable
 *
 * @description
 *
 * Add value i to the current value in the atomic variable.
 *
 * @return none
 */
static inline void __attribute__ ((always_inline))
lw_atomic64_add(LW_INOUT lw_atomic64_t *atomic, 
                LW_IN lw_uint64_t i)
{
    lw_uint64_lock_add(&atomic->val, i);
}

/**
 * Add to an atomic variable (64-bit)
 *
 * @param atomic (i) the input parameter specifying a pointer to an
 *                   atomic variable
 * @param i (i) value to be added to the atomic variable
 *
 * @description
 *
 * Add value i to the current value in the atomic variable.
 *
 * @return Value following addition
 */
static inline lw_uint64_t __attribute__ ((always_inline))
lw_atomic64_add_with_ret(LW_INOUT lw_atomic64_t *atomic, 
                         LW_IN lw_uint64_t i)
{
    return lw_uint64_lock_xadd(&atomic->val, i) + i;
}

/**
 * Subtract from an atomic variable (64-bit)
 *
 * @param atomic (i) the input parameter specifying a pointer to an
 *                   atomic variable
 * @param i (i) value to be subtracted to the atomic variable
 *
 * @description
 *
 * Subtract value i from the current value in the atomic variable.
 *
 * @return none
 */
static inline void __attribute__ ((always_inline))
lw_atomic64_sub(lw_atomic64_t *atomic, lw_uint64_t i)
{
    lw_uint64_lock_sub(&atomic->val, i);
}

/**
 * Subtract from an atomic variable (64-bit)
 *
 * @param atomic (i) the input parameter specifying a pointer to an
 *                   atomic variable
 * @param i (i) value to be subtracted to the atomic variable
 *
 * @description
 *
 * Subtract value i from the current value in the atomic variable.
 *
 * @return Value following subtraction
 */
static inline lw_uint64_t __attribute__ ((always_inline))
lw_atomic64_sub_with_ret(lw_atomic64_t *atomic, lw_uint64_t i)
{
    return lw_uint64_lock_xsub(&atomic->val, i) - i;
}

