/* ============================================================================
 * AzamiOS — Security Hardening Implementation
 * File: kernel/security/security.c
 * ============================================================================ */

#define DEBUG 1
#include <azami/debug.h>
#include "security.h"
#include "../../arch/x86_64/cpu/msr.h"
#include "../../drivers/char/console.h"
#include "../../include/azami/defs.h"


/* Global stack canary guard value (randomized via RDRAND if supported) */
uintptr_t __stack_chk_guard = 0x595E9FBD94FDA766ULL;

void security_init(void)
{
    /* Try to generate a cryptographic random canary via RDRAND */
    u64 rand_val = 0;
    unsigned char ok = 0;

    /* Check if RDRAND supported via CPUID leaf 1 ECX bit 30 */
    u32 eax, ebx, ecx, edx;
    cpuid(1, 0, &eax, &ebx, &ecx, &edx);
    if (ecx & (1U << 30)) {
        __asm__ volatile("rdrand %0; setc %1" : "=r"(rand_val), "=qm"(ok));
        if (ok && rand_val != 0) {
            __stack_chk_guard = rand_val;
        }
    }

    pr_debug("[SECURITY] Stack canary guard active: 0x%016llx\n",
            (unsigned long long)__stack_chk_guard);
}

__noreturn void __stack_chk_fail(void)
{
    PANIC("KERNEL SECURITY VIOLATION: Stack Canary Check Failed! (Buffer Overflow Detected)");
}

bool security_validate_user_ptr(const void *ptr, size_t size)
{
    uintptr_t addr = (uintptr_t)ptr;
    /* User space must reside below the canonical hole (< 0x00007FFFFFFFFFFF) */
    if (addr >= 0x0000800000000000ULL) return false;
    if (addr + size < addr || addr + size >= 0x0000800000000000ULL) return false;
    return true;
}

bool security_validate_kernel_ptr(const void *ptr, size_t size)
{
    uintptr_t addr = (uintptr_t)ptr;
    /* Kernel space must reside in the higher half (>= 0xFFFF800000000000) */
    if (addr < 0xFFFF800000000000ULL) return false;
    if (addr + size < addr) return false;
    return true;
}

bool security_check_permission(process_t *proc, u32 capability)
{
    if (!proc) return false;
    if (proc->pid == 1 || proc->pid == 0) return true; /* Kernel/init always allowed */
    (void)capability;
    return true;
}
