/* ============================================================================
 * AzamiOS Userspace — x86_64 CPUID & Extensions Intrinsic Header (cpuid.h)
 * File: userland/libc/include/cpuid.h
 * ============================================================================ */
#pragma once

/* ── Standard Feature Flags (CPUID Leaf 1 EDX) ────────────────────────────── */
#define bit_FPU         (1U << 0)
#define bit_VME         (1U << 1)
#define bit_DE          (1U << 2)
#define bit_PSE         (1U << 3)
#define bit_TSC         (1U << 4)
#define bit_MSR         (1U << 5)
#define bit_PAE         (1U << 6)
#define bit_MCE         (1U << 7)
#define bit_CX8         (1U << 8)
#define bit_APIC        (1U << 9)
#define bit_SEP         (1U << 11)
#define bit_MTRR        (1U << 12)
#define bit_PGE         (1U << 13)
#define bit_MCA         (1U << 14)
#define bit_CMOV        (1U << 15)
#define bit_PAT         (1U << 16)
#define bit_PSE36       (1U << 17)
#define bit_CLFSH       (1U << 19)
#define bit_MMX         (1U << 23)
#define bit_FXSR        (1U << 24)
#define bit_SSE         (1U << 25)
#define bit_SSE2        (1U << 26)
#define bit_HTT         (1U << 28)

/* ── Standard Feature Flags (CPUID Leaf 1 ECX) ────────────────────────────── */
#define bit_SSE3        (1U << 0)
#define bit_PCLMULQDQ   (1U << 1)
#define bit_DTES64      (1U << 2)
#define bit_MONITOR     (1U << 3)
#define bit_SSSE3       (1U << 9)
#define bit_FMA         (1U << 12)
#define bit_CMPXCHG16B  (1U << 13)
#define bit_PCID        (1U << 17)
#define bit_SSE4_1      (1U << 19)
#define bit_SSE4_2      (1U << 20)
#define bit_X2APIC      (1U << 21)
#define bit_MOVBE       (1U << 22)
#define bit_POPCNT      (1U << 23)
#define bit_TSC_DEADLINE (1U << 24)
#define bit_AES         (1U << 25)
#define bit_XSAVE       (1U << 26)
#define bit_OSXSAVE     (1U << 27)
#define bit_AVX         (1U << 28)
#define bit_F16C        (1U << 29)
#define bit_RDRAND      (1U << 30)

/* ── Extended Feature Flags (CPUID Leaf 7 Subleaf 0 EBX) ──────────────────── */
#define bit_FSGSBASE    (1U << 0)
#define bit_TSC_ADJUST  (1U << 1)
#define bit_SGX         (1U << 2)
#define bit_BMI         (1U << 3)
#define bit_HLE         (1U << 4)
#define bit_AVX2        (1U << 5)
#define bit_SMEP        (1U << 7)
#define bit_BMI2        (1U << 8)
#define bit_ERMS        (1U << 9)
#define bit_INVPCID     (1U << 10)
#define bit_RTM         (1U << 11)
#define bit_MPX         (1U << 14)
#define bit_AVX512F     (1U << 16)
#define bit_AVX512DQ    (1U << 17)
#define bit_RDSEED      (1U << 18)
#define bit_ADX         (1U << 19)
#define bit_SMAP        (1U << 20)
#define bit_AVX512IFMA  (1U << 21)
#define bit_CLFLUSHOPT  (1U << 23)
#define bit_CLWB        (1U << 24)
#define bit_SHA         (1U << 29)
#define bit_AVX512BW    (1U << 30)
#define bit_AVX512VL    (1U << 31)

/* ── Extended Feature Flags (CPUID Leaf 7 Subleaf 0 ECX) ──────────────────── */
#define bit_PREFETCHWT1 (1U << 0)
#define bit_AVX512VBMI  (1U << 1)
#define bit_UMIP        (1U << 2)
#define bit_PKU         (1U << 3)
#define bit_OSPKE       (1U << 4)
#define bit_WAITPKG     (1U << 5)
#define bit_AVX512VBMI2 (1U << 6)
#define bit_GFNI        (1U << 8)
#define bit_VAES        (1U << 9)
#define bit_VPCLMULQDQ  (1U << 10)
#define bit_AVX512VNNI  (1U << 11)
#define bit_AVX512BITALG (1U << 12)
#define bit_AVX512VPOPCNTDQ (1U << 14)
#define bit_RDPID       (1U << 22)

/* ── Extended Function Flags (CPUID Leaf 0x80000001 EDX/ECX) ──────────────── */
#define bit_SYSCALL     (1U << 11)
#define bit_NX          (1U << 20)
#define bit_1GB_PAGE    (1U << 26)
#define bit_RDTSCP      (1U << 27)
#define bit_LM          (1U << 29)
#define bit_LAHF_LM     (1U << 0)
#define bit_ABM         (1U << 5)
#define bit_SSE4a       (1U << 6)
#define bit_3DNOWPREFETCH (1U << 8)

/* ── GCC/Clang Compatible Functions ───────────────────────────────────────── */

static inline void __cpuid(unsigned int __leaf,
                           unsigned int *__eax, unsigned int *__ebx,
                           unsigned int *__ecx, unsigned int *__edx)
{
    __asm__ volatile("cpuid"
                     : "=a"(*__eax), "=b"(*__ebx), "=c"(*__ecx), "=d"(*__edx)
                     : "a"(__leaf), "c"(0));
}

static inline void __cpuid_count(unsigned int __leaf, unsigned int __subleaf,
                                 unsigned int *__eax, unsigned int *__ebx,
                                 unsigned int *__ecx, unsigned int *__edx)
{
    __asm__ volatile("cpuid"
                     : "=a"(*__eax), "=b"(*__ebx), "=c"(*__ecx), "=d"(*__edx)
                     : "a"(__leaf), "c"(__subleaf));
}

static inline unsigned int __get_cpuid_max(unsigned int __ext, unsigned int *__sig)
{
    unsigned int __eax, __ebx, __ecx, __edx;
    __cpuid(__ext, &__eax, &__ebx, &__ecx, &__edx);
    if (__sig) *__sig = __ebx;
    return __eax;
}

static inline int __get_cpuid(unsigned int __leaf,
                              unsigned int *__eax, unsigned int *__ebx,
                              unsigned int *__ecx, unsigned int *__edx)
{
    unsigned int __ext = __leaf & 0x80000000;
    unsigned int __max = __get_cpuid_max(__ext, 0);

    if (__max == 0 || __max < __leaf) return 0;

    __cpuid(__leaf, __eax, __ebx, __ecx, __edx);
    return 1;
}

static inline int __get_cpuid_count(unsigned int __leaf, unsigned int __subleaf,
                                    unsigned int *__eax, unsigned int *__ebx,
                                    unsigned int *__ecx, unsigned int *__edx)
{
    unsigned int __ext = __leaf & 0x80000000;
    unsigned int __max = __get_cpuid_max(__ext, 0);

    if (__max == 0 || __max < __leaf) return 0;

    __cpuid_count(__leaf, __subleaf, __eax, __ebx, __ecx, __edx);
    return 1;
}
