/**
 * sys/cpu.h — x86 CPUID Capability & Topology Checker for AzamiOS Userspace
 *
 * EDUCATIONAL ARCHITECTURE & SECURITY EXPLANATIONS:
 * 1. Unprivileged CPUID Querying:
 *    On x86 processors, the CPUID instruction (0x0F 0xA2) is unprivileged (can be
 *    executed at Ring 3). This allows userspace applications (Shell, System Monitor)
 *    to inspect processor topology and hardware capabilities without trapping into Ring 0.
 * 2. Register Preservation & Memory Safety:
 *    Inline assembly queries pass variables directly via input/output constraints.
 *    String formatting for CPU vendor names ensures null-termination within fixed 16-byte buffers.
 */

#ifndef _SYS_CPU_H
#define _SYS_CPU_H

#include <stdint.h>

typedef struct {
    char vendor[16];
    uint32_t family;
    uint32_t model;
    uint32_t stepping;
    int has_fpu;
    int has_tsc;
    int has_msr;
    int has_pae;
    int has_apic;
    int has_sse;
    int has_sse2;
} cpu_info_t;

static inline void get_cpu_info(cpu_info_t *info) {
    if (!info) return;
    
    uint32_t eax, ebx, ecx, edx;
    
    /* Leaf 0: Vendor ID */
    asm volatile("cpuid"
                 : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                 : "a"(0));
                 
    info->vendor[0] = (char)(ebx & 0xFF);
    info->vendor[1] = (char)((ebx >> 8) & 0xFF);
    info->vendor[2] = (char)((ebx >> 16) & 0xFF);
    info->vendor[3] = (char)((ebx >> 24) & 0xFF);
    info->vendor[4] = (char)(edx & 0xFF);
    info->vendor[5] = (char)((edx >> 8) & 0xFF);
    info->vendor[6] = (char)((edx >> 16) & 0xFF);
    info->vendor[7] = (char)((edx >> 24) & 0xFF);
    info->vendor[8] = (char)(ecx & 0xFF);
    info->vendor[9] = (char)((ecx >> 8) & 0xFF);
    info->vendor[10] = (char)((ecx >> 16) & 0xFF);
    info->vendor[11] = (char)((ecx >> 24) & 0xFF);
    info->vendor[12] = '\0';
    
    /* Leaf 1: Family, Model, Stepping, Features */
    asm volatile("cpuid"
                 : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                 : "a"(1));
                 
    info->stepping = eax & 0xF;
    info->model = (eax >> 4) & 0xF;
    info->family = (eax >> 8) & 0xF;
    if (info->family == 0xF) {
        info->family += (eax >> 20) & 0xFF;
    }
    if (info->family >= 0x6) {
        info->model += ((eax >> 16) & 0xF) << 4;
    }
    
    info->has_fpu  = (edx & (1 << 0)) ? 1 : 0;
    info->has_tsc  = (edx & (1 << 4)) ? 1 : 0;
    info->has_msr  = (edx & (1 << 5)) ? 1 : 0;
    info->has_pae  = (edx & (1 << 6)) ? 1 : 0;
    info->has_apic = (edx & (1 << 9)) ? 1 : 0;
    info->has_sse  = (edx & (1 << 25)) ? 1 : 0;
    info->has_sse2 = (edx & (1 << 26)) ? 1 : 0;
}

#endif /* _SYS_CPU_H */
