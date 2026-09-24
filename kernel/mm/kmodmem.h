/* ============================================================================
 * AzamiOS — Executable Kernel Code Allocator (W^X)
 * File: kernel/mm/kmodmem.h
 *
 * Kernel code that writes machine code at runtime — here, only the BPF JIT —
 * needs memory it can first write and then execute. Doing that with kmalloc()
 * gets both at once and gets them permanently: the heap lives in the HHDM,
 * which before kprotect_seal() was writable *and* executable everywhere, so a
 * JIT buffer was an attacker-writable page the kernel would happily jump to,
 * and every other heap object shared the same property.
 *
 * This allocator separates the two permissions in time. An allocation starts
 * out RW and NX at a dedicated kernel virtual address, is filled in, and is
 * then sealed to RX by kmod_seal_exec() — which drops WRITE in the same edit
 * that grants execute, so the two are never set together for one instant.
 * The frames come from the PMM and therefore still have an HHDM alias; that
 * alias is NX for the whole window after kprotect_seal(), so it is a writable
 * view of the bytes and never a second way to run them.
 *
 * The region is its own slice of kernel address space rather than part of the
 * heap, which keeps the "can this address hold code?" question answerable by
 * looking at the address.
 * ============================================================================ */
#pragma once

#include "../../include/azami/types.h"

/**
 * kmod_alloc_exec(len) — reserve @len bytes of future kernel code.
 *
 * Returns a page-aligned, zeroed, writable, non-executable pointer, or NULL.
 * The caller writes the code and then calls kmod_seal_exec() before making
 * any call through it. Calling into an unsealed allocation faults.
 */
void *kmod_alloc_exec(size_t len);

/**
 * kmod_seal_exec(p) — make an allocation executable and stop it being
 * writable, in one page-table edit. Returns 0, or -1 if @p is not a live
 * unsealed allocation.
 */
int kmod_seal_exec(void *p);

/**
 * kmod_free_exec(p) — unmap an allocation and return its frames to the PMM.
 * Safe on NULL. Safe whether or not the allocation was sealed.
 */
void kmod_free_exec(void *p);

/**
 * kmod_is_exec_addr(p) — true when @p lies inside the executable code window.
 * Lets a caller tell a JIT image apart from a kmalloc() pointer on a free
 * path that can receive either.
 */
bool kmod_is_exec_addr(const void *p);

/** Bytes currently reserved by live allocations, for /proc/meminfo. */
u64 kmod_exec_bytes(void);

/**
 * kmod_selftest_prepare() / kmod_selftest_verify() — two halves of one test.
 *
 * prepare() runs just *before* kprotect_seal(): it allocates, writes and
 * seals a tiny generated function, which is all of the page-table work.
 * verify() runs just *after*: it re-checks the permissions, calls the
 * function, and frees it. Splitting them keeps a burst of global TLB
 * shootdowns off the moment the seal has just issued one of its own — see
 * the comment above the implementation — while still proving the thing that
 * matters, that generated code runs with the whole direct map non-executable.
 *
 * verify() leaves the page mapped; kmod_selftest_release() frees it, and is
 * called at the very end of kernel_main() so that its unmap's TLB shootdown
 * does not land on the heels of the seal's own. See the comment in the
 * implementation for what that cost when it did.
 *
 * Each logs its own failure reason and returns false.
 */
bool kmod_selftest_prepare(void);
bool kmod_selftest_verify(void);
void kmod_selftest_release(void);
