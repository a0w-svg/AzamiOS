/**
 * cpu_check.c — Ring 0 x86 CPU Capability & Topology Checker Subsystem
 *
 * EDUCATIONAL ARCHITECTURE & SECURITY EXPLANATIONS:
 * 1. Hardware Capability Verification:
 *    Before executing advanced kernel subsystems (like SMP multiprocessing or SIMD
 *    floating point math), the operating system must verify that the underlying physical
 *    or virtual CPU supports required features (APIC, TSC, MSR, PAE, SSE).
 * 2. Modularity & Isolation:
 *    Implementing CPU verification as an autonomous kernel subsystem allows early
 *    detection of hardware incompatibilities during the modular bootstrap sequence,
 *    preventing unrecoverable undefined instruction faults (#UD) later in runtime.
 */

#include "include/cpu_check.h"
#include "../klibc/include/stdio.h"
#include <stdint.h>

void cpu_check_init(void) {
    uint32_t eax, ebx, ecx, edx;
    char vendor[13];

    /* Leaf 0: Vendor ID */
    asm volatile("cpuid"
                 : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                 : "a"(0));

    vendor[0]  = (char)(ebx & 0xFF);
    vendor[1]  = (char)((ebx >> 8) & 0xFF);
    vendor[2]  = (char)((ebx >> 16) & 0xFF);
    vendor[3]  = (char)((ebx >> 24) & 0xFF);
    vendor[4]  = (char)(edx & 0xFF);
    vendor[5]  = (char)((edx >> 8) & 0xFF);
    vendor[6]  = (char)((edx >> 16) & 0xFF);
    vendor[7]  = (char)((edx >> 24) & 0xFF);
    vendor[8]  = (char)(ecx & 0xFF);
    vendor[9]  = (char)((ecx >> 8) & 0xFF);
    vendor[10] = (char)((ecx >> 16) & 0xFF);
    vendor[11] = (char)((ecx >> 24) & 0xFF);
    vendor[12] = '\0';

    /* Leaf 1: Family, Model, Stepping, Features */
    asm volatile("cpuid"
                 : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                 : "a"(1));

    uint32_t stepping = eax & 0xF;
    uint32_t model    = (eax >> 4) & 0xF;
    uint32_t family   = (eax >> 8) & 0xF;
    if (family == 0xF) family += (eax >> 20) & 0xFF;
    if (family >= 0x6) model  += ((eax >> 16) & 0xF) << 4;

    kprintf("cpu: Vendor [%s] Family %d Model %d Stepping %d\n", vendor, family, model, stepping);
    
    kprintf("cpu: Features detected:");
    if (edx & (1 << 0))  kprintf(" [FPU]");
    if (edx & (1 << 4))  kprintf(" [TSC]");
    if (edx & (1 << 5))  kprintf(" [MSR]");
    if (edx & (1 << 6))  kprintf(" [PAE]");
    if (edx & (1 << 9))  kprintf(" [APIC]");
    if (edx & (1 << 13)) kprintf(" [PGE]");
    if (edx & (1 << 23)) kprintf(" [MMX]");
    if (edx & (1 << 25)) kprintf(" [SSE]");
    if (edx & (1 << 26)) kprintf(" [SSE2]");
    kprintf("\n");
}
