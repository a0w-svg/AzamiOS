/* ============================================================================
 * AzamiOS — Spectre-v1 Bounds-Check Bypass Barriers (x86_64)
 * File: arch/x86_64/cpu/nospec.h
 *
 * A bounds check is a conditional branch, and a conditional branch can be
 * mispredicted. `if (n < size) return table[n];` therefore still speculatively
 * loads table[n] for an out-of-range n, and the cache footprint of that load
 * survives the rollback — which is the whole of Spectre-v1.
 *
 * The fix is not a fence (too slow to put on every array access) but a mask
 * computed from the *same* comparison, using the carry flag rather than a
 * branch. The CPU cannot speculate past a data dependency, so an out-of-range
 * index becomes 0 in the speculative path as surely as in the architectural
 * one.
 *
 * Use it wherever a value that crossed the user/kernel boundary indexes an
 * array — syscall numbers, file descriptors, ioctl codes. It is nearly free:
 * two ALU ops with no branch and no serialisation.
 * ============================================================================ */
#pragma once

#include "../../../include/azami/types.h"

/**
 * array_index_mask_nospec(index, size) — all-ones when index < size, else zero.
 *
 * CMP sets CF exactly when the unsigned comparison borrows, i.e. index < size;
 * SBB of a register from itself then materialises -CF, which is 0 or ~0. The
 * result depends on the comparison through the data path, not through a
 * predicted branch, which is what makes it a barrier.
 */
static __attribute__((always_inline)) inline
unsigned long array_index_mask_nospec(unsigned long index, unsigned long size)
{
    unsigned long mask;
    __asm__ volatile("cmp %1, %2; sbb %0, %0"
                     : "=r" (mask)
                     : "g" (size), "r" (index)
                     : "cc");
    return mask;
}

/**
 * array_index_nospec(index, size) — @index clamped to 0 when out of range.
 *
 * Does NOT replace the bounds check: callers must still reject the
 * out-of-range case. This only guarantees that the speculative path uses a
 * safe index while the architectural path is on its way to rejecting it.
 */
#define array_index_nospec(index, size)                                 \
({                                                                      \
    __typeof__(index) _i = (index);                                         \
    __typeof__(size)  _s = (size);                                          \
    unsigned long _mask = array_index_mask_nospec((unsigned long)_i,    \
                                                  (unsigned long)_s);   \
    (__typeof__(_i))((unsigned long)_i & _mask);                            \
})

/** barrier_nospec() — stop speculation outright. For the rare site where a
 *  mask does not fit (a pointer that must not be dereferenced speculatively at
 *  all). Costs a pipeline drain, so it is not the default tool. */
static __attribute__((always_inline)) inline void barrier_nospec(void)
{
    __asm__ volatile("lfence" ::: "memory");
}
