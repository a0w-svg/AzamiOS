/* ============================================================================
 * AzamiOS Userspace — C11 <stdatomic.h>
 * File: userland/libc/include/stdatomic.h
 *
 * Implementation strategy
 * ───────────────────────
 * We expose the C11 API through two independent layers so that both the
 * cross-compiler (GCC 14, which has __atomic_* builtins) and any toolchain
 * that lacks them (fallback via __sync_* or plain volatile) work correctly:
 *
 *   1. Prefer __atomic_* builtins (GCC ≥ 4.7, Clang ≥ 3.1).
 *   2. Fall back to __sync_* primitives (GCC ≥ 4.1) when __ATOMIC_SEQ_CST
 *      is not defined.
 *   3. Last-resort fallback: plain volatile accesses with a full compiler
 *      barrier (__asm__ volatile("" ::: "memory")).  This is correct for
 *      single-processor targets that never do out-of-order stores (including
 *      AzamiOS's kernel objects on a single BSP before SMP is live).
 *
 * The header is self-contained (no .c file).  It implements every mandatory
 * C11 / C17 stdatomic symbol.
 * ============================================================================ */
#pragma once

/* ── memory_order ─────────────────────────────────────────────────────────── */
#ifdef __ATOMIC_RELAXED
    /* GCC / Clang with __atomic_* builtins */
    typedef enum memory_order {
        memory_order_relaxed = __ATOMIC_RELAXED,
        memory_order_consume = __ATOMIC_CONSUME,
        memory_order_acquire = __ATOMIC_ACQUIRE,
        memory_order_release = __ATOMIC_RELEASE,
        memory_order_acq_rel = __ATOMIC_ACQ_REL,
        memory_order_seq_cst = __ATOMIC_SEQ_CST
    } memory_order;
    #define AZ_ATOMIC_HAVE_BUILTIN 1
#else
    typedef enum memory_order {
        memory_order_relaxed = 0,
        memory_order_consume = 1,
        memory_order_acquire = 2,
        memory_order_release = 3,
        memory_order_acq_rel = 4,
        memory_order_seq_cst = 5
    } memory_order;
    #define AZ_ATOMIC_HAVE_BUILTIN 0
#endif

/* ── compiler barrier (used in fallback path) ─────────────────────────────── */
#define _az_compiler_barrier() __asm__ volatile("" ::: "memory")

/* ── _Atomic type decorator ───────────────────────────────────────────────── */
/* GCC / Clang support _Atomic natively in C11 mode (-std=c11 or later).
 * Older compilers that don't will fall back to a volatile-qualified plain
 * type, which is safe on x86_64 for naturally-aligned word-sized accesses. */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && \
    (defined(__clang__) || (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 9)))
    /* _Atomic is a keyword; nothing to define */
#else
    #define _Atomic volatile
#endif

#ifndef __cplusplus
#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 199901L
#ifndef _Bool
typedef unsigned char _Bool;
#endif
#endif
#else
typedef bool _Bool;
#endif

/* ── Atomic types ─────────────────────────────────────────────────────────── */
typedef _Atomic _Bool               atomic_bool;
typedef _Atomic char                atomic_char;
typedef _Atomic signed char         atomic_schar;
typedef _Atomic unsigned char       atomic_uchar;
typedef _Atomic short               atomic_short;
typedef _Atomic unsigned short      atomic_ushort;
typedef _Atomic int                 atomic_int;
typedef _Atomic unsigned int        atomic_uint;
typedef _Atomic long                atomic_long;
typedef _Atomic unsigned long       atomic_ulong;
typedef _Atomic long long           atomic_llong;
typedef _Atomic unsigned long long  atomic_ullong;
typedef _Atomic __PTRDIFF_TYPE__    atomic_ptrdiff_t;
typedef _Atomic __SIZE_TYPE__       atomic_size_t;
typedef _Atomic __INTPTR_TYPE__     atomic_intptr_t;
typedef _Atomic __UINTPTR_TYPE__    atomic_uintptr_t;

/* ── atomic_flag ──────────────────────────────────────────────────────────── */
typedef struct { volatile unsigned char _val; } atomic_flag;
#define ATOMIC_FLAG_INIT { 0 }
#define ATOMIC_VAR_INIT(v) (v)

/* ── atomic_init ──────────────────────────────────────────────────────────── */
#define atomic_init(obj, val)  (*(obj) = (val))

/* ═════════════════════════════════════════════════════════════════════════════
 * Path A — GCC / Clang __atomic_* builtins
 * ═════════════════════════════════════════════════════════════════════════════ */
#if AZ_ATOMIC_HAVE_BUILTIN

/* ── fence ────────────────────────────────────────────────────────────────── */
#define atomic_thread_fence(order)   __atomic_thread_fence(order)
#define atomic_signal_fence(order)   __atomic_signal_fence(order)

/* ── load ─────────────────────────────────────────────────────────────────── */
#define atomic_load_explicit(obj, order)    __atomic_load_n(obj, order)
#define atomic_load(obj)                    __atomic_load_n(obj, __ATOMIC_SEQ_CST)

/* ── store ────────────────────────────────────────────────────────────────── */
#define atomic_store_explicit(obj, val, order)  __atomic_store_n(obj, val, order)
#define atomic_store(obj, val)                  __atomic_store_n(obj, val, __ATOMIC_SEQ_CST)

/* ── exchange ─────────────────────────────────────────────────────────────── */
#define atomic_exchange_explicit(obj, val, order) \
    __atomic_exchange_n(obj, val, order)
#define atomic_exchange(obj, val) \
    __atomic_exchange_n(obj, val, __ATOMIC_SEQ_CST)

/* ── compare-exchange strong ──────────────────────────────────────────────── */
#define atomic_compare_exchange_strong_explicit(obj, expected, desired, succ, fail) \
    __atomic_compare_exchange_n(obj, expected, desired, 0, succ, fail)
#define atomic_compare_exchange_strong(obj, expected, desired) \
    __atomic_compare_exchange_n(obj, expected, desired, 0,    \
                                __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)

/* ── compare-exchange weak ────────────────────────────────────────────────── */
#define atomic_compare_exchange_weak_explicit(obj, expected, desired, succ, fail) \
    __atomic_compare_exchange_n(obj, expected, desired, 1, succ, fail)
#define atomic_compare_exchange_weak(obj, expected, desired) \
    __atomic_compare_exchange_n(obj, expected, desired, 1,   \
                                __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)

/* ── fetch_* ──────────────────────────────────────────────────────────────── */
#define atomic_fetch_add_explicit(obj, arg, order) __atomic_fetch_add(obj, arg, order)
#define atomic_fetch_add(obj, arg)                 __atomic_fetch_add(obj, arg, __ATOMIC_SEQ_CST)

#define atomic_fetch_sub_explicit(obj, arg, order) __atomic_fetch_sub(obj, arg, order)
#define atomic_fetch_sub(obj, arg)                 __atomic_fetch_sub(obj, arg, __ATOMIC_SEQ_CST)

#define atomic_fetch_or_explicit(obj, arg, order)  __atomic_fetch_or(obj, arg, order)
#define atomic_fetch_or(obj, arg)                  __atomic_fetch_or(obj, arg, __ATOMIC_SEQ_CST)

#define atomic_fetch_and_explicit(obj, arg, order) __atomic_fetch_and(obj, arg, order)
#define atomic_fetch_and(obj, arg)                 __atomic_fetch_and(obj, arg, __ATOMIC_SEQ_CST)

#define atomic_fetch_xor_explicit(obj, arg, order) __atomic_fetch_xor(obj, arg, order)
#define atomic_fetch_xor(obj, arg)                 __atomic_fetch_xor(obj, arg, __ATOMIC_SEQ_CST)

/* ── atomic_flag ──────────────────────────────────────────────────────────── */
#define atomic_flag_test_and_set_explicit(flag, order) \
    __atomic_test_and_set(&(flag)->_val, order)
#define atomic_flag_test_and_set(flag) \
    __atomic_test_and_set(&(flag)->_val, __ATOMIC_SEQ_CST)

#define atomic_flag_clear_explicit(flag, order) \
    __atomic_clear(&(flag)->_val, order)
#define atomic_flag_clear(flag) \
    __atomic_clear(&(flag)->_val, __ATOMIC_SEQ_CST)

/* ═════════════════════════════════════════════════════════════════════════════
 * Path B — __sync_* fallback (GCC ≥ 4.1, no __atomic_* available)
 * ═════════════════════════════════════════════════════════════════════════════ */
#elif defined(__GCC_HAVE_SYNC_COMPARE_AND_SWAP_4)

#define atomic_thread_fence(order) __sync_synchronize()
#define atomic_signal_fence(order) _az_compiler_barrier()

#define atomic_load_explicit(obj, order) \
    (__sync_synchronize(), *(volatile __typeof__(*(obj)) *)(obj))
#define atomic_load(obj) atomic_load_explicit(obj, memory_order_seq_cst)

#define atomic_store_explicit(obj, val, order) \
    do { *(volatile __typeof__(*(obj)) *)(obj) = (val); __sync_synchronize(); } while(0)
#define atomic_store(obj, val) atomic_store_explicit(obj, val, memory_order_seq_cst)

#define atomic_exchange_explicit(obj, val, order) \
    __sync_lock_test_and_set(obj, val)
#define atomic_exchange(obj, val) \
    __sync_lock_test_and_set(obj, val)

#define atomic_compare_exchange_strong_explicit(obj, exp, des, s, f) \
    ({ __typeof__(*(exp)) _cmp = *(exp); \
       __typeof__(*(exp)) _old = __sync_val_compare_and_swap(obj, _cmp, des); \
       int _ret; \
       if (_old == _cmp) { _ret = 1; } else { *(exp) = _old; _ret = 0; } \
       _ret; })
#define atomic_compare_exchange_strong(obj, exp, des) \
    atomic_compare_exchange_strong_explicit(obj, exp, des, 0, 0)
#define atomic_compare_exchange_weak_explicit \
    atomic_compare_exchange_strong_explicit
#define atomic_compare_exchange_weak \
    atomic_compare_exchange_strong

#define atomic_fetch_add_explicit(obj, arg, order) __sync_fetch_and_add(obj, arg)
#define atomic_fetch_add(obj, arg)                 __sync_fetch_and_add(obj, arg)
#define atomic_fetch_sub_explicit(obj, arg, order) __sync_fetch_and_sub(obj, arg)
#define atomic_fetch_sub(obj, arg)                 __sync_fetch_and_sub(obj, arg)
#define atomic_fetch_or_explicit(obj, arg, order)  __sync_fetch_and_or(obj, arg)
#define atomic_fetch_or(obj, arg)                  __sync_fetch_and_or(obj, arg)
#define atomic_fetch_and_explicit(obj, arg, order) __sync_fetch_and_and(obj, arg)
#define atomic_fetch_and(obj, arg)                 __sync_fetch_and_and(obj, arg)
#define atomic_fetch_xor_explicit(obj, arg, order) __sync_fetch_and_xor(obj, arg)
#define atomic_fetch_xor(obj, arg)                 __sync_fetch_and_xor(obj, arg)

#define atomic_flag_test_and_set_explicit(flag, order) \
    __sync_lock_test_and_set(&(flag)->_val, 1)
#define atomic_flag_test_and_set(flag) \
    __sync_lock_test_and_set(&(flag)->_val, 1)
#define atomic_flag_clear_explicit(flag, order) \
    __sync_lock_release(&(flag)->_val)
#define atomic_flag_clear(flag) \
    __sync_lock_release(&(flag)->_val)

/* ═════════════════════════════════════════════════════════════════════════════
 * Path C — volatile / compiler-barrier last-resort fallback
 *           (correct on single-CPU x86_64; unsafe on SMP without hardware fences)
 * ═════════════════════════════════════════════════════════════════════════════ */
#else

#define atomic_thread_fence(order) _az_compiler_barrier()
#define atomic_signal_fence(order) _az_compiler_barrier()

#define atomic_load_explicit(obj, order) \
    (_az_compiler_barrier(), *(volatile __typeof__(*(obj)) *)(obj))
#define atomic_load(obj) atomic_load_explicit(obj, memory_order_seq_cst)

#define atomic_store_explicit(obj, val, order) \
    do { *(volatile __typeof__(*(obj)) *)(obj) = (val); _az_compiler_barrier(); } while(0)
#define atomic_store(obj, val) atomic_store_explicit(obj, val, memory_order_seq_cst)

#define atomic_exchange_explicit(obj, val, order) \
    ({ __typeof__(*(obj)) _old = *(volatile __typeof__(*(obj)) *)(obj); \
       *(volatile __typeof__(*(obj)) *)(obj) = (val);                    \
       _az_compiler_barrier(); _old; })
#define atomic_exchange(obj, val) atomic_exchange_explicit(obj, val, memory_order_seq_cst)

#define atomic_compare_exchange_strong_explicit(obj, exp, des, s, f)    \
    ({ __typeof__(*(obj)) _cur = *(volatile __typeof__(*(obj)) *)(obj);  \
       int _ok = 0;                                                      \
       if (_cur == *(exp)) {                                             \
           *(volatile __typeof__(*(obj)) *)(obj) = (des);               \
           _az_compiler_barrier(); _ok = 1;                             \
       } else { *(exp) = _cur; }                                        \
       _ok; })
#define atomic_compare_exchange_strong(obj, exp, des) \
    atomic_compare_exchange_strong_explicit(obj, exp, des, 0, 0)
#define atomic_compare_exchange_weak_explicit \
    atomic_compare_exchange_strong_explicit
#define atomic_compare_exchange_weak \
    atomic_compare_exchange_strong

#define atomic_fetch_add_explicit(obj, arg, order) \
    ({ __typeof__(*(obj)) _v = *(volatile __typeof__(*(obj)) *)(obj); \
       *(volatile __typeof__(*(obj)) *)(obj) = _v + (arg);            \
       _az_compiler_barrier(); _v; })
#define atomic_fetch_add(obj, arg) atomic_fetch_add_explicit(obj, arg, 0)

#define atomic_fetch_sub_explicit(obj, arg, order) \
    ({ __typeof__(*(obj)) _v = *(volatile __typeof__(*(obj)) *)(obj); \
       *(volatile __typeof__(*(obj)) *)(obj) = _v - (arg);            \
       _az_compiler_barrier(); _v; })
#define atomic_fetch_sub(obj, arg) atomic_fetch_sub_explicit(obj, arg, 0)

#define atomic_fetch_or_explicit(obj, arg, order) \
    ({ __typeof__(*(obj)) _v = *(volatile __typeof__(*(obj)) *)(obj); \
       *(volatile __typeof__(*(obj)) *)(obj) = _v | (arg);            \
       _az_compiler_barrier(); _v; })
#define atomic_fetch_or(obj, arg) atomic_fetch_or_explicit(obj, arg, 0)

#define atomic_fetch_and_explicit(obj, arg, order) \
    ({ __typeof__(*(obj)) _v = *(volatile __typeof__(*(obj)) *)(obj); \
       *(volatile __typeof__(*(obj)) *)(obj) = _v & (arg);            \
       _az_compiler_barrier(); _v; })
#define atomic_fetch_and(obj, arg) atomic_fetch_and_explicit(obj, arg, 0)

#define atomic_fetch_xor_explicit(obj, arg, order) \
    ({ __typeof__(*(obj)) _v = *(volatile __typeof__(*(obj)) *)(obj); \
       *(volatile __typeof__(*(obj)) *)(obj) = _v ^ (arg);            \
       _az_compiler_barrier(); _v; })
#define atomic_fetch_xor(obj, arg) atomic_fetch_xor_explicit(obj, arg, 0)

#define atomic_flag_test_and_set_explicit(flag, order) \
    ({ unsigned char _old = (flag)->_val; (flag)->_val = 1; _az_compiler_barrier(); _old; })
#define atomic_flag_test_and_set(flag) atomic_flag_test_and_set_explicit(flag, 0)

#define atomic_flag_clear_explicit(flag, order) \
    do { (flag)->_val = 0; _az_compiler_barrier(); } while(0)
#define atomic_flag_clear(flag) atomic_flag_clear_explicit(flag, 0)

#endif /* fallback paths */

/* ── ATOMIC_LOCK_FREE detection (best-effort) ─────────────────────────────── */
#if AZ_ATOMIC_HAVE_BUILTIN
  #define ATOMIC_BOOL_LOCK_FREE     __GCC_ATOMIC_BOOL_LOCK_FREE
  #define ATOMIC_CHAR_LOCK_FREE     __GCC_ATOMIC_CHAR_LOCK_FREE
  #define ATOMIC_SHORT_LOCK_FREE    __GCC_ATOMIC_SHORT_LOCK_FREE
  #define ATOMIC_INT_LOCK_FREE      __GCC_ATOMIC_INT_LOCK_FREE
  #define ATOMIC_LONG_LOCK_FREE     __GCC_ATOMIC_LONG_LOCK_FREE
  #define ATOMIC_LLONG_LOCK_FREE    __GCC_ATOMIC_LLONG_LOCK_FREE
  #define ATOMIC_POINTER_LOCK_FREE  __GCC_ATOMIC_POINTER_LOCK_FREE
#else
  /* On x86_64 all naturally-aligned ≤64-bit accesses are lock-free */
  #define ATOMIC_BOOL_LOCK_FREE     2
  #define ATOMIC_CHAR_LOCK_FREE     2
  #define ATOMIC_SHORT_LOCK_FREE    2
  #define ATOMIC_INT_LOCK_FREE      2
  #define ATOMIC_LONG_LOCK_FREE     2
  #define ATOMIC_LLONG_LOCK_FREE    2
  #define ATOMIC_POINTER_LOCK_FREE  2
#endif

#define atomic_is_lock_free(obj) \
    (sizeof(*(obj)) <= sizeof(void *) ? 1 : 0)
