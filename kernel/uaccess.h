/* ============================================================================
 * AzamiOS — User Space Memory Access
 * File: kernel/uaccess.h
 *
 * Provides safe functions for copying data to and from user space,
 * respecting SMAP and providing fault recovery via the exception table.
 * ============================================================================ */
#pragma once

#include "../include/azami/types.h"
#include "../include/azami/defs.h"

/* The exception table entry maps a faulting instruction address (insn)
 * to a recovery address (fixup) to handle page faults gracefully. */
typedef struct {
    u64 insn;
    u64 fixup;
} extable_entry_t;

extern u8 g_smap_enabled;
extern u8 g_smep_enabled;

/**
 * user_access_begin() — Temporarily allow kernel access to user space (STAC).
 */
static inline void user_access_begin(void) {
    if (g_smap_enabled) {
        __asm__ volatile("stac" ::: "cc");
    }
}

/**
 * user_access_end() — Re-enable kernel protection against user access (CLAC).
 */
static inline void user_access_end(void) {
    if (g_smap_enabled) {
        __asm__ volatile("clac" ::: "cc");
    }
}

/**
 * copy_from_user(dst, src, size) — Safely copy memory from user space.
 * @dst  Kernel destination buffer.
 * @src  User source buffer.
 * @size Number of bytes to copy.
 * 
 * Returns the number of bytes that could NOT be copied (0 on success).
 */
size_t copy_from_user(void *dst, const void *src, size_t size);

/**
 * copy_to_user(dst, src, size) — Safely copy memory to user space.
 * @dst  User destination buffer.
 * @src  Kernel source buffer.
 * @size Number of bytes to copy.
 *
 * Returns the number of bytes that could NOT be copied (0 on success).
 */
size_t copy_to_user(void *dst, const void *src, size_t size);

/**
 * search_extable(ip) — Search the exception table for a faulting instruction.
 * @ip   The instruction pointer (RIP) that caused the page fault.
 * 
 * Returns the fixup address if found, or 0 if not found.
 */
u64 search_extable(u64 ip);

/**
 * check_zeroed_user(addr, size) — Check whether a user memory range contains only zeros.
 * @addr  User source buffer.
 * @size  Number of bytes to verify.
 *
 * Returns:
 *   1 if non-zero bytes are found,
 *   0 if all bytes are zero,
 *   -EFAULT if an invalid address was encountered.
 */
int check_zeroed_user(const void *addr, size_t size);

/**
 * copy_struct_from_user(dst, ksize, src, usize) — Extensible-struct protocol copy.
 * @dst   Kernel destination buffer.
 * @ksize Kernel struct size (sizeof(*dst)).
 * @src   User source buffer.
 * @usize Size passed by user space.
 *
 * Implements Linux extensible-struct copy semantics:
 * - If usize == 0: returns -EINVAL.
 * - If usize > 4096 (PAGE_SIZE): returns -E2BIG.
 * - If src is NULL or not in user space: returns -EFAULT.
 * - If usize > ksize: ensures trailing (usize - ksize) bytes in src are zero.
 *   If non-zero bytes found: returns -E2BIG. If fault: returns -EFAULT.
 * - If usize < ksize: zeroes the tail (ksize - usize) in dst.
 * - Copies min(ksize, usize) bytes from src to dst. Returns -EFAULT on failure.
 *
 * Returns 0 on success, or negative errno (-EINVAL, -E2BIG, -EFAULT).
 */
int copy_struct_from_user(void *dst, size_t ksize, const void *src, size_t usize);


