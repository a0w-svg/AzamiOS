/*
 * =========================================================================================
 * Azami OS — Hardware Abstraction Layer (HAL) CPU Management Module
 * File: /kernel/hal/cpu/cpu.c
 * Architecture: x86_64 (64-Bit Long Mode exclusively)
 *
 * Description:
 *   This module implements low-level CPU feature verification, floating-point and
 *   SIMD instruction enabling, and Model-Specific Register (MSR) configuration.
 *
 * Educational Step-by-Step Overview:
 *   When the CPU starts up or switches into 64-bit Long Mode, several advanced
 *   instructions and hardware extensions remain disabled or locked by default:
 *     1. Long Mode Feature Verification (`CPUID 0x80000001`): Even though the bootloader
 *        transitioned us to 64-bit mode, robust systems software verifies the `LM` (bit 29)
 *        and `NX` (No-Execute bit 20) capabilities to ensure page tables can enforce data isolation.
 *     2. SIMD / SSE Enablement (`CR4.OSFXSR` & `CR4.OSXMMEXCPT`): Modern C compilers
 *        use `XMM` registers (`xmm0` through `xmm15`) to pass floating-point arguments
 *        and optimize memory copies. If `OSFXSR` (bit 9) and `OSXMMEXCPT` (bit 10) in
 *        register `CR4` are not set by ring 0 code, executing any SSE instruction will immediately
 *        trigger an Invalid Opcode (`#UD` vector 6) exception!
 *     3. System Call Extensions (`IA32_EFER.SCE`): To support fast `syscall` and `sysretq`
 *        instructions (bypassing slow software interrupt gates like `int 0x80`), bit 0 (`SCE`)
 *        of the Extended Feature Enable Register (`EFER`, MSR `0xC0000080`) must be explicitly set.
 * =========================================================================================
 */

#include "../include/hal.h"
#include "../../klibc/include/stdio.h"

/* =========================================================================================
 * MSR HELPER IMPLEMENTATIONS
 * ========================================================================================= */

uint64_t hal_cpu_get_msr(uint32_t msr)
{
    /* Delegate directly to our low-level assembly wrapper (`rdmsr`) */
    return hal_cpu_rdmsr(msr);
}

void hal_cpu_set_msr(uint32_t msr, uint64_t value)
{
    /* Delegate directly to our low-level assembly wrapper (`wrmsr`) */
    hal_cpu_wrmsr(msr, value);
}

/* =========================================================================================
 * CPU INITIALIZATION & FEATURE VALIDATION
 * ========================================================================================= */

void hal_cpu_init(void)
{
    uint32_t eax, ebx, ecx, edx;

    /*
     * Step 1: Verify Extended CPUID Leaf Availability
     * We execute CPUID with `eax = 0x80000000` to determine the maximum supported
     * extended function leaf on this processor.
     */
    hal_cpu_cpuid(0x80000000, 0, &eax, &ebx, &ecx, &edx);
    if (eax < 0x80000001) {
        kprintf("[HAL-CPU] CRITICAL: CPUID leaf 0x80000001 not supported on this processor!\n");
        return;
    }

    /*
     * Step 2: Query Extended Features (Leaf 0x80000001)
     *   - `EDX` bit 29 (`LM`): Long Mode (64-Bit architecture support)
     *   - `EDX` bit 11 (`SYSCALL`): Support for SYSCALL/SYSRET instructions
     *   - `EDX` bit 20 (`NX`): No-Execute page protection bit support
     */
    hal_cpu_cpuid(0x80000001, 0, &eax, &ebx, &ecx, &edx);
    bool has_long_mode = (edx & (1UL << 29)) != 0;
    bool has_syscall   = (edx & (1UL << 11)) != 0;
    bool has_nx_bit    = (edx & (1UL << 20)) != 0;

    if (!has_long_mode) {
        kprintf("[HAL-CPU] CRITICAL: Processor lacks 64-bit Long Mode (`LM`) support!\n");
        return;
    }

    /*
     * Step 3: Enable OS Support for FXSAVE/FXRSTOR and Unmasked SSE/AVX Exceptions (`CR4`)
     * Why are we touching `CR4`?
     *   If an application or kernel function uses SSE (`xmm0-xmm15`) without `CR4` bit 9
     *   (`OSFXSR` = OS FXSAVE/FXRSTOR Support) and bit 10 (`OSXMMEXCPT` = OS Unmasked Exception
     *   Support) turned on, the CPU will raise `#UD` (Invalid Opcode).
     */
    uint64_t cr4 = hal_cpu_get_cr4();
    cr4 |= HAL_CR4_OSFXSR | HAL_CR4_OSXMMEXCPT;
    hal_cpu_set_cr4(cr4);

    /*
     * Step 4: Verify and Configure `IA32_EFER` MSR (Extended Feature Enable Register)
     * We read `0xC0000080` to verify Long Mode is active (`LMA` bit 10) and enable
     * System Call Extensions (`SCE` bit 0) and No-Execute (`NXE` bit 11) if supported.
     */
    uint64_t efer = hal_cpu_rdmsr(HAL_MSR_IA32_EFER);

    /* Ensure Long Mode Enable (`LME`) and Long Mode Active (`LMA`) are asserted */
    if (!(efer & HAL_EFER_LMA)) {
        kprintf("[HAL-CPU] WARNING: EFER.LMA (Long Mode Active) bit is not set in MSR 0xC0000080!\n");
    }

    /* Enable `SYSCALL` / `SYSRET` instruction extensions (`SCE` bit 0) */
    if (has_syscall) {
        efer |= HAL_EFER_SCE;
    }

    /* Enable No-Execute page table protection bit (`NXE` bit 11) if hardware supports it */
    if (has_nx_bit) {
        efer |= HAL_EFER_NXE;
    }

    hal_cpu_wrmsr(HAL_MSR_IA32_EFER, efer);

    /* Log successful verification and initialization of Core CPU features */
    kprintf("[HAL-CPU] x86_64 CPU features initialized: LM=Yes, SYSCALL=%s, NXE=%s, CR4.OSFXSR=Yes\n",
            has_syscall ? "Yes" : "No",
            has_nx_bit  ? "Yes" : "No");
}
