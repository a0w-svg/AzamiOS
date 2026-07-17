/**
 * access_control.h  -  Ring-based access control for AzamiOS
 *
 * Rules:
 *  - Ring 0 (kernel) can touch everything.
 *  - Ring 3 (user)   cannot touch kernel memory or I/O ports.
 *  - Syscall ptrs from userspace validated before dereference.
 *
 * Works for both x86 (32-bit) and x86_64 (64-bit).
 */
#ifndef ACCESS_CONTROL_H
#define ACCESS_CONTROL_H

#include <stdint.h>
#include <stdbool.h>
#include "../arch/include/isr.h"

/* ── Address space boundaries ─────────────────────────────────────────────── */

/*  Canonical user range: 0x0000_0000_0000_0000 – 0x0000_7FFF_FFFF_FFFF
 *  Kernel lives above:   0xFFFF_8000_0000_0000 and up                    */
#define USER_SPACE_TOP   ((uintptr_t)0x0000800000000000ULL)
#define KERNEL_SPACE_BOT ((uintptr_t)0xFFFF800000000000ULL)

/* ── Privilege level ──────────────────────────────────────────────────────── */

#define RING_KERNEL 0
#define RING_USER   3

/**
 * get_current_ring - extract CPL from saved CS register.
 * CS bits [1:0] = RPL = current privilege level.
 */
static inline int get_current_ring(const registers_t *r) {
    return (int)(r->cs & 3);
}

/* ── Address validation ───────────────────────────────────────────────────── */

/**
 * is_user_address - true if entire [ptr, ptr+size) lives in user space.
 * Rejects NULL, kernel pointers, and wraps.
 */
static inline bool is_user_address(uintptr_t ptr, uintptr_t size) {
    if (ptr == 0) return false;
    if (ptr >= USER_SPACE_TOP) return false;             /* in kernel range */
    if (size > 0 && (ptr + size) < ptr) return false;   /* overflow */
    if (size > 0 && (ptr + size) > USER_SPACE_TOP) return false; /* straddles */
    return true;
}

/**
 * is_kernel_address - true if ptr lives in kernel address space.
 */
static inline bool is_kernel_address(uintptr_t ptr) {
    return (ptr >= KERNEL_SPACE_BOT);
}

/* ── I/O port access control ─────────────────────────────────────────────── */

/**
 * io_port_allowed - Check if process is allowed to access an I/O port.
 * Ring 0 kernel processes can access all ports.
 * Ring 3 userspace drivers (like AzamiOS GPU, audio, and gameport drivers in user/libc/)
 * are permitted access to specific hardware port ranges (PCI config, VGA/VBE, PIT/Speaker, Gameport).
 */
static inline bool io_port_allowed(const registers_t *r, uint16_t port) {
    if (get_current_ring(r) == RING_KERNEL) {
        return true;
    }
    /* Allowlist for AzamiOS ring-3 userspace drivers */
    if (port >= 0xCF8 && port <= 0xCFF) return true; /* PCI Config Space (gpu.c, intel_gpu.c, amd_gpu.c) */
    if (port >= 0x3C0 && port <= 0x3DF) return true; /* VGA / VBE Display registers */
    if (port == 0x1CE || port == 0x1CF) return true; /* Bochs VBE Dispi registers */
    if (port >= 0x40 && port <= 0x43)   return true; /* PIT Timer registers (speaker.c) */
    if (port == 0x61 || port == 0x201)  return true; /* PC Speaker & Gameport (speaker.c, gameport.c) */
    if (port == 0x60 || port == 0x64)   return true; /* PS/2 Keyboard & Mouse */
    return false;
}

/* ── Syscall pointer validation ──────────────────────────────────────────── */

/**
 * syscall_validate_ptr - validate a user-supplied pointer before kernel dereference.
 *
 * @r    : register frame (used to get CPL)
 * @ptr  : pointer from userspace
 * @size : number of bytes caller intends to access (0 = just check not NULL+kernel)
 *
 * Returns true if safe to use.
 * Returns false if pointer is bad (logs warning).
 *
 * NOTE: If CPL==0 (kernel calling kernel) we skip the user-range check.
 *       Kernel code is trusted.
 */
bool syscall_validate_ptr(const registers_t *r, uintptr_t ptr, uintptr_t size);

/* ── Process kill helper (forward-declared; defined in access_control.c) ─── */

/**
 * ac_kill_current_process - mark current process dead + schedule next.
 * Used by page fault handler when ring-3 does a bad memory access.
 * If no other process is runnable, halts CPU.
 */
void ac_kill_current_process(const char *reason);

/**
 * ac_demand_page - try to satisfy a not-present user page fault by
 *                  allocating and mapping a fresh physical frame.
 *
 * @fault_addr : the virtual address that caused the fault (from CR2)
 *
 * Returns true  if page was successfully allocated and mapped.
 * Returns false if PMM is out of memory or address is not in user range.
 */
bool ac_demand_page(uintptr_t fault_addr);

#endif /* ACCESS_CONTROL_H */
