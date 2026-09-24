/* ============================================================================
 * AzamiOS — Architecture & Hardware Instruction Diagnostics Tool
 * File: userland/apps/testarch/main.c
 *
 * Probes and benchmarks CPU instruction set extensions, vector capabilities,
 * cryptographic acceleration, bit manipulation, and cache management.
 * ============================================================================ */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

static inline void cpuid(uint32_t leaf, uint32_t subleaf, uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx)
{
    __asm__ volatile("cpuid"
                     : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                     : "a"(leaf), "c"(subleaf));
}

static inline uint64_t rdtsc_read(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static inline uint64_t xgetbv_read(uint32_t ecx)
{
    uint32_t lo, hi;
    __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(ecx));
    return ((uint64_t)hi << 32) | lo;
}

typedef struct {
    char vendor[13];
    char brand[49];
    uint32_t family;
    uint32_t model;
    uint32_t stepping;

    /* Vector & SIMD */
    bool sse;
    bool sse2;
    bool sse3;
    bool ssse3;
    bool sse4_1;
    bool sse4_2;
    bool avx;
    bool avx2;
    bool fma;
    bool f16c;
    bool avx512f;
    bool avx512dq;
    bool avx512cd;
    bool avx512bw;
    bool avx512vl;

    /* Bit Manipulation */
    bool popcnt;
    bool lzcnt;
    bool bmi1;
    bool bmi2;

    /* Cryptography */
    bool aesni;
    bool pclmulqdq;
    bool sha;
    bool vaes;
    bool vpclmulqdq;

    /* Security & RNG */
    bool rdrand;
    bool rdseed;

    /* Memory / Cache / Execution */
    bool fsgsbase;
    bool erms;
    bool fsrm;
    bool clflush;
    bool clflushopt;
    bool clwb;
    bool waitpkg;
    bool serialize;

    /* OS enablement */
    bool osxsave;
    bool os_avx_enabled;
    bool os_avx512_enabled;
} cpu_caps_t;

static void probe_cpu(cpu_caps_t *caps)
{
    memset(caps, 0, sizeof(*caps));

    uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
    cpuid(0, 0, &eax, &ebx, &ecx, &edx);
    uint32_t max_leaf = eax;

    /* Vendor string: EBX, EDX, ECX */
    *(uint32_t *)&caps->vendor[0] = ebx;
    *(uint32_t *)&caps->vendor[4] = edx;
    *(uint32_t *)&caps->vendor[8] = ecx;
    caps->vendor[12] = '\0';

    if (max_leaf >= 1) {
        cpuid(1, 0, &eax, &ebx, &ecx, &edx);

        caps->stepping = eax & 0xF;
        uint32_t base_model = (eax >> 4) & 0xF;
        uint32_t base_family = (eax >> 8) & 0xF;
        uint32_t ext_model = (eax >> 16) & 0xF;
        uint32_t ext_family = (eax >> 20) & 0xFF;

        caps->family = (base_family == 0xF) ? (base_family + ext_family) : base_family;
        caps->model = (base_family == 0x6 || base_family == 0xF) ? ((ext_model << 4) | base_model) : base_model;

        caps->sse       = (edx & (1u << 25)) != 0;
        caps->sse2      = (edx & (1u << 26)) != 0;
        caps->clflush   = (edx & (1u << 19)) != 0;

        caps->sse3      = (ecx & (1u << 0)) != 0;
        caps->pclmulqdq = (ecx & (1u << 1)) != 0;
        caps->ssse3     = (ecx & (1u << 9)) != 0;
        caps->fma       = (ecx & (1u << 12)) != 0;
        caps->sse4_1    = (ecx & (1u << 19)) != 0;
        caps->sse4_2    = (ecx & (1u << 20)) != 0;
        caps->popcnt    = (ecx & (1u << 23)) != 0;
        caps->aesni     = (ecx & (1u << 25)) != 0;
        caps->osxsave   = (ecx & (1u << 27)) != 0;
        caps->avx       = (ecx & (1u << 28)) != 0;
        caps->f16c      = (ecx & (1u << 29)) != 0;
        caps->rdrand    = (ecx & (1u << 30)) != 0;
    }

    if (max_leaf >= 7) {
        cpuid(7, 0, &eax, &ebx, &ecx, &edx);

        caps->fsgsbase   = (ebx & (1u << 0)) != 0;
        caps->bmi1       = (ebx & (1u << 3)) != 0;
        caps->avx2       = (ebx & (1u << 5)) != 0;
        caps->bmi2       = (ebx & (1u << 8)) != 0;
        caps->erms       = (ebx & (1u << 9)) != 0;
        caps->avx512f    = (ebx & (1u << 16)) != 0;
        caps->avx512dq   = (ebx & (1u << 17)) != 0;
        caps->rdseed     = (ebx & (1u << 18)) != 0;
        caps->clflushopt = (ebx & (1u << 23)) != 0;
        caps->clwb       = (ebx & (1u << 24)) != 0;
        caps->avx512cd   = (ebx & (1u << 28)) != 0;
        caps->sha        = (ebx & (1u << 29)) != 0;
        caps->avx512bw   = (ebx & (1u << 30)) != 0;
        caps->avx512vl   = (ebx & (1u << 31)) != 0;

        caps->waitpkg    = (ecx & (1u << 5)) != 0;
        caps->vaes       = (ecx & (1u << 9)) != 0;
        caps->vpclmulqdq = (ecx & (1u << 10)) != 0;

        caps->fsrm       = (edx & (1u << 4)) != 0;
        caps->serialize  = (edx & (1u << 14)) != 0;
    }

    /* Extended leaves */
    cpuid(0x80000000, 0, &eax, &ebx, &ecx, &edx);
    uint32_t max_ext = eax;

    if (max_ext >= 0x80000001) {
        cpuid(0x80000001, 0, &eax, &ebx, &ecx, &edx);
        caps->lzcnt = (ecx & (1u << 5)) != 0;
    }

    if (max_ext >= 0x80000004) {
        uint32_t *b = (uint32_t *)caps->brand;
        cpuid(0x80000002, 0, &b[0], &b[1], &b[2], &b[3]);
        cpuid(0x80000003, 0, &b[4], &b[5], &b[6], &b[7]);
        cpuid(0x80000004, 0, &b[8], &b[9], &b[10], &b[11]);
        caps->brand[48] = '\0';
    } else {
        snprintf(caps->brand, sizeof(caps->brand), "x86_64 Processor");
    }

    /* Check OSXSAVE and XCR0 for OS AVX/AVX-512 state enabling */
    if (caps->osxsave) {
        uint64_t xcr0 = xgetbv_read(0);
        caps->os_avx_enabled = (xcr0 & 0x6) == 0x6;
        caps->os_avx512_enabled = (xcr0 & 0xE6) == 0xE6;
    }
}

static void print_caps(const cpu_caps_t *caps, bool verbose)
{
    printf("\033[1;36m=== AzamiOS Hardware Instruction & Architecture Report ===\033[0m\n\n");
    printf("\033[1mProcessor:\033[0m  %s\n", caps->brand);
    printf("\033[1mVendor:\033[0m     %s\n", caps->vendor);
    printf("\033[1mSignature:\033[0m  Family 0x%X, Model 0x%X, Stepping 0x%X\n\n",
           caps->family, caps->model, caps->stepping);

    printf("\033[1;33m[Vector & SIMD Instruction Sets]\033[0m\n");
    printf("  SSE:       %s    SSE2:      %s    SSE3:      %s\n",
           caps->sse ? "\033[32mYES\033[0m" : "\033[31mNO\033[0m ",
           caps->sse2 ? "\033[32mYES\033[0m" : "\033[31mNO\033[0m ",
           caps->sse3 ? "\033[32mYES\033[0m" : "\033[31mNO\033[0m ");
    printf("  SSSE3:     %s    SSE4.1:    %s    SSE4.2:    %s\n",
           caps->ssse3 ? "\033[32mYES\033[0m" : "\033[31mNO\033[0m ",
           caps->sse4_1 ? "\033[32mYES\033[0m" : "\033[31mNO\033[0m ",
           caps->sse4_2 ? "\033[32mYES\033[0m" : "\033[31mNO\033[0m ");
    printf("  AVX:       %s    AVX2:      %s    FMA:       %s\n",
           caps->avx ? (caps->os_avx_enabled ? "\033[32mYES (active)\033[0m" : "\033[33mHW only\033[0m     ") : "\033[31mNO\033[0m          ",
           caps->avx2 ? (caps->os_avx_enabled ? "\033[32mYES (active)\033[0m" : "\033[33mHW only\033[0m     ") : "\033[31mNO\033[0m          ",
           caps->fma ? "\033[32mYES\033[0m" : "\033[31mNO\033[0m ");
    printf("  AVX-512:   %s    (F:%s DQ:%s BW:%s VL:%s)\n",
           caps->avx512f ? (caps->os_avx512_enabled ? "\033[32mYES\033[0m" : "\033[33mHW only\033[0m") : "\033[31mNO\033[0m",
           caps->avx512f ? "1" : "0", caps->avx512dq ? "1" : "0",
           caps->avx512bw ? "1" : "0", caps->avx512vl ? "1" : "0");

    printf("\n\033[1;33m[Bit Manipulation & Arithmetic]\033[0m\n");
    printf("  POPCNT:    %s    LZCNT:     %s\n",
           caps->popcnt ? "\033[32mYES\033[0m" : "\033[31mNO\033[0m",
           caps->lzcnt ? "\033[32mYES\033[0m" : "\033[31mNO\033[0m");
    printf("  BMI1:      %s    (ANDN, BEXTR, BLSI, BLSMSK, BLSR, TZCNT)\n",
           caps->bmi1 ? "\033[32mYES\033[0m" : "\033[31mNO\033[0m");
    printf("  BMI2:      %s    (BZHI, MULX, PDEP, PEXT, RORX, SARX, SHLX, SHRX)\n",
           caps->bmi2 ? "\033[32mYES\033[0m" : "\033[31mNO\033[0m");

    printf("\n\033[1;33m[Cryptographic & Random Acceleration]\033[0m\n");
    printf("  AES-NI:    %s    PCLMULQDQ: %s    SHA-NI:    %s\n",
           caps->aesni ? "\033[32mYES\033[0m" : "\033[31mNO\033[0m",
           caps->pclmulqdq ? "\033[32mYES\033[0m" : "\033[31mNO\033[0m",
           caps->sha ? "\033[32mYES\033[0m" : "\033[31mNO\033[0m");
    printf("  RDRAND:    %s    RDSEED:    %s    VAES:      %s\n",
           caps->rdrand ? "\033[32mYES\033[0m" : "\033[31mNO\033[0m",
           caps->rdseed ? "\033[32mYES\033[0m" : "\033[31mNO\033[0m",
           caps->vaes ? "\033[32mYES\033[0m" : "\033[31mNO\033[0m");

    printf("\n\033[1;33m[Memory, Cache & Execution Control]\033[0m\n");
    printf("  ERMS:      %s    FSRM:      %s    FSGSBASE:  %s\n",
           caps->erms ? "\033[32mYES\033[0m" : "\033[31mNO\033[0m",
           caps->fsrm ? "\033[32mYES\033[0m" : "\033[31mNO\033[0m",
           caps->fsgsbase ? "\033[32mYES\033[0m" : "\033[31mNO\033[0m");
    printf("  CLFLUSH:   %s    CLFLUSHOPT:%s    CLWB:      %s\n",
           caps->clflush ? "\033[32mYES\033[0m" : "\033[31mNO\033[0m",
           caps->clflushopt ? "\033[32mYES\033[0m" : "\033[31mNO\033[0m",
           caps->clwb ? "\033[32mYES\033[0m" : "\033[31mNO\033[0m");
    printf("  WAITPKG:   %s    SERIALIZE: %s\n",
           caps->waitpkg ? "\033[32mYES\033[0m" : "\033[31mNO\033[0m",
           caps->serialize ? "\033[32mYES\033[0m" : "\033[31mNO\033[0m");

    if (verbose) {
        printf("\n\033[1;35m[CPUID Raw Register State]\033[0m\n");
        uint32_t a, b, c, d;
        cpuid(1, 0, &a, &b, &c, &d);
        printf("  Leaf 0x01:   EAX=%08x EBX=%08x ECX=%08x EDX=%08x\n", a, b, c, d);
        cpuid(7, 0, &a, &b, &c, &d);
        printf("  Leaf 0x07:0: EAX=%08x EBX=%08x ECX=%08x EDX=%08x\n", a, b, c, d);
    }
}

__attribute__((target("avx,avx2")))
static void bench_avx(void)
{
    float a[8] __attribute__((aligned(32))) = { 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f };
    float b[8] __attribute__((aligned(32))) = { 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f };
    uint64_t t0 = rdtsc_read();
    for (int i = 0; i < 1000000; i++) {
        __asm__ volatile(
            "vmovaps %0, %%ymm0\n"
            "vmovaps %1, %%ymm1\n"
            "vaddps %%ymm1, %%ymm0, %%ymm2\n"
            "vmulps %%ymm2, %%ymm0, %%ymm0\n"
            : : "m"(a), "m"(b) : "ymm0", "ymm1", "ymm2"
        );
    }
    uint64_t t1 = rdtsc_read();
    printf("  \033[32m[AVX/AVX2]\033[0m 1M 256-bit VADDPS+VMULPS (8M FLOPs): %lu cycles (avg %.2f cycles/vec)\n",
           t1 - t0, (double)(t1 - t0) / 1000000.0);
}

__attribute__((target("fma")))
static void bench_fma(void)
{
    float a[8] __attribute__((aligned(32))) = { 1.1f, 2.2f, 3.3f, 4.4f, 5.5f, 6.6f, 7.7f, 8.8f };
    float b[8] __attribute__((aligned(32))) = { 0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f };
    uint64_t t0 = rdtsc_read();
    for (int i = 0; i < 1000000; i++) {
        __asm__ volatile(
            "vmovaps %0, %%ymm0\n"
            "vmovaps %1, %%ymm1\n"
            "vfmadd213ps %%ymm1, %%ymm0, %%ymm0\n"
            : : "m"(a), "m"(b) : "ymm0", "ymm1"
        );
    }
    uint64_t t1 = rdtsc_read();
    printf("  \033[32m[FMA3]\033[0m     1M 256-bit VFMADD213PS (16M FLOPs): %lu cycles (avg %.2f cycles/op)\n",
           t1 - t0, (double)(t1 - t0) / 1000000.0);
}

__attribute__((target("aes")))
static void bench_aes(void)
{
    uint64_t t0 = rdtsc_read();
    for (int i = 0; i < 1000000; i++) {
        __asm__ volatile(
            "pxor %%xmm0, %%xmm0\n"
            "pxor %%xmm1, %%xmm1\n"
            "aesenc %%xmm1, %%xmm0\n"
            : : : "xmm0", "xmm1"
        );
    }
    uint64_t t1 = rdtsc_read();
    printf("  \033[32m[AES-NI]\033[0m   1M AESENC rounds: %lu cycles (avg %.2f cycles/round)\n",
           t1 - t0, (double)(t1 - t0) / 1000000.0);
}

__attribute__((target("sha")))
static void bench_sha(void)
{
    uint64_t t0 = rdtsc_read();
    for (int i = 0; i < 1000000; i++) {
        __asm__ volatile(
            "pxor %%xmm0, %%xmm0\n"
            "pxor %%xmm1, %%xmm1\n"
            "sha256rnds2 %%xmm0, %%xmm1, %%xmm0\n"
            : : : "xmm0", "xmm1"
        );
    }
    uint64_t t1 = rdtsc_read();
    printf("  \033[32m[SHA-NI]\033[0m   1M SHA256RNDS2 steps (2M SHA rounds): %lu cycles (avg %.2f cycles/step)\n",
           t1 - t0, (double)(t1 - t0) / 1000000.0);
}

__attribute__((target("sse")))
static void bench_3d_transform(void)
{
    float mat[16] __attribute__((aligned(16))) = {
        1.5f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.5f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.5f, 5.0f,
        0.0f, 0.0f, -1.0f, 0.0f
    };
    float in_v[4] __attribute__((aligned(16))) = { 1.2f, 3.4f, 5.6f, 1.0f };
    float out_v[4] __attribute__((aligned(16)));

    /* Scalar baseline */
    uint64_t t0 = rdtsc_read();
    float x = in_v[0], y = in_v[1], z = in_v[2], w = in_v[3];
    for (int i = 0; i < 200000; i++) {
        float ox = mat[0]*x + mat[1]*y + mat[2]*z + mat[3]*w;
        float oy = mat[4]*x + mat[5]*y + mat[6]*z + mat[7]*w;
        float oz = mat[8]*x + mat[9]*y + mat[10]*z + mat[11]*w;
        float ow = mat[12]*x + mat[13]*y + mat[14]*z + mat[15]*w;
        out_v[0] = ox / ow;
        out_v[1] = oy / ow;
        out_v[2] = oz / ow;
        out_v[3] = 1.0f;
    }
    uint64_t t1 = rdtsc_read();
    uint64_t scalar_cycles = t1 - t0;

    /* SIMD accelerated */
    t0 = rdtsc_read();
    for (int i = 0; i < 200000; i++) {
        __asm__ volatile(
            "movss 0(%1), %%xmm0\n"
            "shufps $0, %%xmm0, %%xmm0\n"
            "mulps 0(%0), %%xmm0\n"

            "movss 4(%1), %%xmm1\n"
            "shufps $0, %%xmm1, %%xmm1\n"
            "mulps 16(%0), %%xmm1\n"
            "addps %%xmm1, %%xmm0\n"

            "movss 8(%1), %%xmm2\n"
            "shufps $0, %%xmm2, %%xmm2\n"
            "mulps 32(%0), %%xmm2\n"
            "addps %%xmm2, %%xmm0\n"

            "movss 12(%1), %%xmm3\n"
            "shufps $0, %%xmm3, %%xmm3\n"
            "mulps 48(%0), %%xmm3\n"
            "addps %%xmm3, %%xmm0\n"

            "movaps %%xmm0, %2\n"
            : : "r"(mat), "r"(in_v), "m"(out_v)
            : "xmm0", "xmm1", "xmm2", "xmm3", "memory"
        );
    }
    t1 = rdtsc_read();
    uint64_t simd_cycles = t1 - t0;

    printf("  \033[32m[3D MVP]\033[0m   200K 4x4 Vertex Transforms: Scalar=%lu cyc, SIMD=%lu cyc (\033[1;32m%.2fx speedup\033[0m)\n",
           scalar_cycles, simd_cycles, (double)scalar_cycles / (double)simd_cycles);
}

static void run_benchmarks(const cpu_caps_t *caps)
{
    printf("\n\033[1;36m=== Hardware Instruction Microbenchmarks ===\033[0m\n\n");

    /* 1. POPCNT test */
    if (caps->popcnt) {
        uint64_t val = 0xAA55AA5512345678ULL;
        uint64_t count = 0;
        uint64_t t0 = rdtsc_read();
        for (int i = 0; i < 1000000; i++) {
            uint64_t c;
            __asm__("popcntq %1, %0" : "=r"(c) : "rm"(val + i));
            count += c;
        }
        uint64_t t1 = rdtsc_read();
        printf("  \033[32m[POPCNT]\033[0m 1M iterations: %lu cycles (avg %.2f cycles/op, accum=%lu)\n",
               t1 - t0, (double)(t1 - t0) / 1000000.0, count);
    } else {
        printf("  \033[33m[POPCNT]\033[0m Skipped (instruction not supported)\n");
    }

    /* 2. BMI1 BEXTR / BLSR */
    if (caps->bmi1) {
        uint64_t val = 0xFEDCBA9876543210ULL;
        uint64_t accum = 0;
        uint64_t control = (4 & 0xFF) | ((16 & 0xFF) << 8);
        uint64_t t0 = rdtsc_read();
        for (int i = 0; i < 1000000; i++) {
            uint64_t r1, r2;
            __asm__("bextrq %2, %1, %0" : "=r"(r1) : "rm"(val + i), "r"(control));
            __asm__("blsrq %1, %0" : "=r"(r2) : "rm"(r1));
            accum += r2;
        }
        uint64_t t1 = rdtsc_read();
        printf("  \033[32m[BMI1]\033[0m   1M BEXTR+BLSR: %lu cycles (avg %.2f cycles/pair)\n",
               t1 - t0, (double)(t1 - t0) / 1000000.0);
    } else {
        printf("  \033[33m[BMI1]\033[0m   Skipped (BMI1 not supported)\n");
    }

    /* 3. BMI2 BZHI / MULX */
    if (caps->bmi2) {
        uint64_t a = 123456789ULL;
        uint64_t b = 987654321ULL;
        uint64_t accum = 0;
        uint64_t t0 = rdtsc_read();
        for (int i = 0; i < 1000000; i++) {
            uint64_t lo, hi;
            __asm__("mulxq %2, %0, %1" : "=r"(lo), "=r"(hi) : "rm"(b + i), "d"(a));
            accum += lo ^ hi;
        }
        uint64_t t1 = rdtsc_read();
        printf("  \033[32m[BMI2]\033[0m   1M MULX 64x64: %lu cycles (avg %.2f cycles/op)\n",
               t1 - t0, (double)(t1 - t0) / 1000000.0);
    } else {
        printf("  \033[33m[BMI2]\033[0m   Skipped (BMI2 not supported)\n");
    }

    /* 4. RDRAND Entropy */
    if (caps->rdrand) {
        uint64_t rnd = 0;
        unsigned char ok = 0;
        __asm__ volatile("rdrand %0; setc %1" : "=r"(rnd), "=qm"(ok));
        if (ok) {
            printf("  \033[32m[RDRAND]\033[0m Hardware entropy generated: 0x%016lx\n", rnd);
        } else {
            printf("  \033[31m[RDRAND]\033[0m Underflow / Retry requested\n");
        }
    }

    /* 5. CRC32C (SSE4.2) */
    if (caps->sse4_2) {
        static char buf[4096];
        memset(buf, 0x5A, sizeof(buf));
        uint32_t crc = 0xFFFFFFFFu;
        uint64_t t0 = rdtsc_read();
        for (int iter = 0; iter < 10000; iter++) {
            uint64_t *p = (uint64_t *)buf;
            uint64_t c64 = crc;
            for (size_t k = 0; k < sizeof(buf) / 8; k++) {
                __asm__("crc32q %1, %0" : "+r"(c64) : "rm"(p[k]));
            }
            crc = (uint32_t)c64;
        }
        uint64_t t1 = rdtsc_read();
        double bytes = (double)sizeof(buf) * 10000.0;
        double mb = bytes / (1024.0 * 1024.0);
        printf("  \033[32m[CRC32C]\033[0m %.1f MB hashed: %lu cycles (%.2f bytes/cycle, digest=0x%08X)\n",
               mb, t1 - t0, bytes / (double)(t1 - t0), crc ^ 0xFFFFFFFFu);
    }

    /* 6. SIMD String / Memory acceleration verification */
    printf("  \033[32m[LIBC]\033[0m   Testing libc SIMD string & reverse scanners...\n");
    static char strbuf[2048];
    memset(strbuf, 'a', sizeof(strbuf) - 1);
    strbuf[sizeof(strbuf) - 1] = '\0';
    strbuf[100] = 'z';
    strbuf[1500] = 'z';

    size_t len = strlen(strbuf);
    void *first_z = memchr(strbuf, 'z', sizeof(strbuf));
    void *last_z = memrchr(strbuf, 'z', sizeof(strbuf));
    char *str_last_z = strrchr(strbuf, 'z');

    if (len == sizeof(strbuf) - 1 &&
        first_z == &strbuf[100] &&
        last_z == &strbuf[1500] &&
        str_last_z == &strbuf[1500]) {
        printf("  \033[32m[PASS]\033[0m   strlen=%zu, first 'z'=%td, last 'z'=%td matched correctly!\n",
               len, (char *)first_z - strbuf, (char *)last_z - strbuf);
    } else {
        printf("  \033[31m[FAIL]\033[0m   Scanner mismatch! len=%zu, first=%p, last=%p\n",
               len, first_z, last_z);
    }

    /* 7. AVX / AVX2 256-bit SIMD */
    if (caps->avx && caps->os_avx_enabled) {
        bench_avx();
    } else {
        printf("  \033[33m[AVX/AVX2]\033[0m Skipped (AVX or OSXSAVE not active)\n");
    }

    /* 8. FMA3 */
    if (caps->fma && caps->os_avx_enabled) {
        bench_fma();
    } else {
        printf("  \033[33m[FMA3]\033[0m     Skipped (FMA not supported)\n");
    }

    /* 9. AES-NI */
    if (caps->aesni) {
        bench_aes();
    } else {
        printf("  \033[33m[AES-NI]\033[0m   Skipped (AES-NI not supported)\n");
    }

    /* 10. SHA-NI */
    if (caps->sha) {
        bench_sha();
    } else {
        printf("  \033[33m[SHA-NI]\033[0m   Skipped (SHA-NI not supported)\n");
    }

    /* 11. 3D Graphics Transform */
    if (caps->sse) {
        bench_3d_transform();
    }
}

int main(int argc, char **argv)
{
    bool verbose = false;
    bool bench = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            verbose = true;
        } else if (strcmp(argv[i], "-b") == 0 || strcmp(argv[i], "--bench") == 0) {
            bench = true;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("Usage: testarch [OPTIONS]\n");
            printf("AzamiOS Hardware Architecture & Instruction Diagnostics\n\n");
            printf("Options:\n");
            printf("  -v, --verbose   Show raw CPUID register contents\n");
            printf("  -b, --bench     Run hardware instruction benchmarks\n");
            printf("  -h, --help      Display this help and exit\n");
            return 0;
        }
    }

    cpu_caps_t caps;
    probe_cpu(&caps);
    print_caps(&caps, verbose);

    if (bench) {
        run_benchmarks(&caps);
    } else {
        printf("\nRun with '\033[1mtestarch --bench\033[0m' to execute hardware instruction benchmarks.\n");
    }

    return 0;
}
