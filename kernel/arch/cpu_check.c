#include "include/cpu_check.h"
#include "../klibc/include/stdio.h"
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    char vendor[13];
    uint32_t family;
    uint32_t model;
    uint32_t stepping;
    uint32_t max_leaf;
    uint32_t max_ext_leaf;
    uint32_t features_edx;
    uint32_t features_ecx;
    uint32_t ext_features_ebx;
    uint32_t ext_features_ecx;
    uint32_t ext8000_features_edx;
    bool valid;
} cpu_caps_t;

static cpu_caps_t g_cpu_caps;

static inline bool cpu_has_cpuid_support(void) {
    uintptr_t flags1, flags2;
    asm volatile(
        "pushf\n\t"
        "pop %0\n\t"
        "mov %0, %1\n\t"
        "xor $0x200000, %0\n\t"
        "push %0\n\t"
        "popf\n\t"
        "pushf\n\t"
        "pop %0\n\t"
        "push %1\n\t"
        "popf\n\t"
        : "=&r"(flags1), "=&r"(flags2)
        :
        : "cc", "memory"
    );
    return ((flags1 ^ flags2) & 0x200000) != 0;
}

static inline void run_cpuid(uint32_t leaf, uint32_t subleaf, uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx) {
    asm volatile("cpuid"
                 : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                 : "a"(leaf), "c"(subleaf));
}

void cpu_check_init(void) {
    if (!cpu_has_cpuid_support()) {
        kprintf("cpu: CPUID instruction not supported by processor.\n");
        return;
    }

    uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
    run_cpuid(0, 0, &eax, &ebx, &ecx, &edx);
    g_cpu_caps.max_leaf = eax;

    g_cpu_caps.vendor[0]  = (char)(ebx & 0xFF);
    g_cpu_caps.vendor[1]  = (char)((ebx >> 8) & 0xFF);
    g_cpu_caps.vendor[2]  = (char)((ebx >> 16) & 0xFF);
    g_cpu_caps.vendor[3]  = (char)((ebx >> 24) & 0xFF);
    g_cpu_caps.vendor[4]  = (char)(edx & 0xFF);
    g_cpu_caps.vendor[5]  = (char)((edx >> 8) & 0xFF);
    g_cpu_caps.vendor[6]  = (char)((edx >> 16) & 0xFF);
    g_cpu_caps.vendor[7]  = (char)((edx >> 24) & 0xFF);
    g_cpu_caps.vendor[8]  = (char)(ecx & 0xFF);
    g_cpu_caps.vendor[9]  = (char)((ecx >> 8) & 0xFF);
    g_cpu_caps.vendor[10] = (char)((ecx >> 16) & 0xFF);
    g_cpu_caps.vendor[11] = (char)((ecx >> 24) & 0xFF);
    g_cpu_caps.vendor[12] = '\0';

    if (g_cpu_caps.max_leaf >= 1) {
        run_cpuid(1, 0, &eax, &ebx, &ecx, &edx);
        g_cpu_caps.stepping = eax & 0xF;
        g_cpu_caps.model    = (eax >> 4) & 0xF;
        g_cpu_caps.family   = (eax >> 8) & 0xF;
        if (g_cpu_caps.family == 0xF) g_cpu_caps.family += (eax >> 20) & 0xFF;
        if (g_cpu_caps.family >= 0x6) g_cpu_caps.model  += ((eax >> 16) & 0xF) << 4;
        g_cpu_caps.features_edx = edx;
        g_cpu_caps.features_ecx = ecx;
    }

    if (g_cpu_caps.max_leaf >= 7) {
        run_cpuid(7, 0, &eax, &ebx, &ecx, &edx);
        g_cpu_caps.ext_features_ebx = ebx;
        g_cpu_caps.ext_features_ecx = ecx;
    }

    run_cpuid(0x80000000, 0, &eax, &ebx, &ecx, &edx);
    g_cpu_caps.max_ext_leaf = eax;
    if (g_cpu_caps.max_ext_leaf >= 0x80000001) {
        run_cpuid(0x80000001, 0, &eax, &ebx, &ecx, &edx);
        g_cpu_caps.ext8000_features_edx = edx;
    }

    g_cpu_caps.valid = true;

    kprintf("cpu: Vendor [%s] Family %u Model %u Stepping %u\n",
            g_cpu_caps.vendor, g_cpu_caps.family, g_cpu_caps.model, g_cpu_caps.stepping);

    kprintf("cpu: Features detected:");
    if (g_cpu_caps.features_edx & (1 << 0))  kprintf(" [FPU]");
    if (g_cpu_caps.features_edx & (1 << 4))  kprintf(" [TSC]");
    if (g_cpu_caps.features_edx & (1 << 5))  kprintf(" [MSR]");
    if (g_cpu_caps.features_edx & (1 << 6))  kprintf(" [PAE]");
    if (g_cpu_caps.features_edx & (1 << 9))  kprintf(" [APIC]");
    if (g_cpu_caps.features_edx & (1 << 13)) kprintf(" [PGE]");
    if (g_cpu_caps.features_edx & (1 << 23)) kprintf(" [MMX]");
    if (g_cpu_caps.features_edx & (1 << 25)) kprintf(" [SSE]");
    if (g_cpu_caps.features_edx & (1 << 26)) kprintf(" [SSE2]");
    if (g_cpu_caps.features_ecx & (1 << 0))  kprintf(" [SSE3]");
    if (g_cpu_caps.features_ecx & (1 << 9))  kprintf(" [SSSE3]");
    if (g_cpu_caps.features_ecx & (1 << 19)) kprintf(" [SSE4.1]");
    if (g_cpu_caps.features_ecx & (1 << 20)) kprintf(" [SSE4.2]");
    if (g_cpu_caps.features_ecx & (1 << 21)) kprintf(" [x2APIC]");
    if (g_cpu_caps.features_ecx & (1 << 28)) kprintf(" [AVX]");
    if (g_cpu_caps.ext_features_ebx & (1 << 0))  kprintf(" [FSGSBASE]");
    if (g_cpu_caps.ext_features_ebx & (1 << 5))  kprintf(" [AVX2]");
    if (g_cpu_caps.ext_features_ebx & (1 << 7))  kprintf(" [SMEP]");
    if (g_cpu_caps.ext_features_ebx & (1 << 20)) kprintf(" [SMAP]");
    if (g_cpu_caps.ext8000_features_edx & (1 << 11)) kprintf(" [SYSCALL]");
    if (g_cpu_caps.ext8000_features_edx & (1 << 20)) kprintf(" [NX]");
    kprintf("\n");
}

