/* ============================================================================
 * AzamiOS — Hardware Instructions & Acceleration Regression Test
 * File: userland/examples/hardware_instructions_test.c
 *
 * Tests hardware instruction primitives (BMI1, BMI2, POPCNT, LZCNT, CRC32, RDRAND)
 * and vectorized string reverse scanners (memrchr, strrchr, rawmemchr, strnlen).
 * ============================================================================ */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* Forward declarations for AzamiOS libc security & CRC functions */
int timingsafe_bcmp(const void *b1, const void *b2, size_t len);
int timingsafe_memcmp(const void *b1, const void *b2, size_t len);
uint32_t crc32c(uint32_t crc, const void *buf, size_t len);

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("[PASS] %s\n", msg); } \
    else { printf("[FAIL] %s\n", msg); g_fail++; } \
} while (0)

static inline void cpuid(uint32_t leaf, uint32_t subleaf, uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx)
{
    __asm__ volatile("cpuid"
                     : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                     : "a"(leaf), "c"(subleaf));
}

static void test_string_simd_scanners(void)
{
    printf("--- SIMD String / Memory Scanners (memrchr, strrchr, rawmemchr, strnlen) ---\n");

    /* 1. memrchr test */
    char buffer[256];
    memset(buffer, 'x', sizeof(buffer));
    buffer[15] = 'Q';
    buffer[42] = 'Q';
    buffer[100] = 'Q';
    buffer[250] = 'Q';

    void *r = memrchr(buffer, 'Q', 256);
    CHECK(r == &buffer[250], "memrchr finds last match across 256 bytes");

    r = memrchr(buffer, 'Q', 250);
    CHECK(r == &buffer[100], "memrchr bounded before last match finds previous");

    r = memrchr(buffer, 'Q', 15);
    CHECK(r == NULL, "memrchr bounded before all matches returns NULL");

    r = memrchr(buffer, 'Z', 256);
    CHECK(r == NULL, "memrchr returns NULL when byte absent");

    /* Test alignment boundary conditions */
    for (int offset = 0; offset < 32; offset++) {
        char small[64];
        memset(small, 0, sizeof(small));
        small[offset] = '!';
        CHECK(memrchr(small, '!', sizeof(small)) == &small[offset], "memrchr unaligned offset");
    }

    /* 2. strrchr test */
    char str[128];
    strcpy(str, "alpha/beta/gamma/delta/epsilon");
    char *p = strrchr(str, '/');
    CHECK(p != NULL && strcmp(p, "/epsilon") == 0, "strrchr finds last slash in path");

    char *term = strrchr(str, '\0');
    CHECK(term == str + strlen(str), "strrchr finds terminating null character");

    CHECK(strrchr(str, 'Z') == NULL, "strrchr returns NULL for missing character");

    /* 3. rawmemchr test */
    char raw[512];
    memset(raw, 'A', sizeof(raw));
    raw[345] = '$';
    void *found = rawmemchr(raw, '$');
    CHECK(found == &raw[345], "rawmemchr locates character in 512-byte buffer");

    /* 4. strnlen test */
    CHECK(strnlen("Hello, world!", 5) == 5, "strnlen clamped to maxlen");
    CHECK(strnlen("Hello", 100) == 5, "strnlen bounded by null terminator");
    CHECK(strnlen("", 10) == 0, "strnlen on empty string is 0");
}

static void test_bitops(void)
{
    printf("\n--- Bit Operations & Instruction Extensions ---\n");

    uint32_t a = 0, b = 0, c = 0, d = 0;
    cpuid(1, 0, &a, &b, &c, &d);
    bool has_popcnt = (c & (1u << 23)) != 0;

    cpuid(7, 0, &a, &b, &c, &d);
    bool has_bmi1 = (b & (1u << 3)) != 0;
    bool has_bmi2 = (b & (1u << 8)) != 0;

    /* POPCNT */
    if (has_popcnt) {
        uint64_t w = 0x8000000000000001ULL;
        uint64_t count;
        __asm__("popcntq %1, %0" : "=r"(count) : "rm"(w));
        CHECK(count == 2, "Hardware POPCNT returns 2 for 0x8000000000000001");

        w = 0xFFFFFFFFFFFFFFFFULL;
        __asm__("popcntq %1, %0" : "=r"(count) : "rm"(w));
        CHECK(count == 64, "Hardware POPCNT returns 64 for full mask");
    } else {
        printf("[SKIP] POPCNT instruction not supported by CPU\n");
    }

    /* BMI1 */
    if (has_bmi1) {
        /* BEXTR: extract bits 4..11 of 0x0000000000000AB0 -> 0xAB */
        uint64_t src = 0x0000000000000AB0ULL;
        uint64_t control = (4 & 0xFF) | ((8 & 0xFF) << 8);
        uint64_t extracted;
        __asm__("bextrq %2, %1, %0" : "=r"(extracted) : "rm"(src), "r"(control));
        CHECK(extracted == 0xAB, "BMI1 BEXTR extracts bitfield [4..12)");

        /* BLSR: clear lowest set bit */
        uint64_t v = 0x18; /* 0b00011000 */
        uint64_t blsr_res;
        __asm__("blsrq %1, %0" : "=r"(blsr_res) : "rm"(v));
        CHECK(blsr_res == 0x10, "BMI1 BLSR clears lowest set bit (0x18 -> 0x10)");

        /* BLSI: extract lowest set bit */
        uint64_t blsi_res;
        __asm__("blsiq %1, %0" : "=r"(blsi_res) : "rm"(v));
        CHECK(blsi_res == 0x08, "BMI1 BLSI extracts lowest set bit (0x18 -> 0x08)");

        /* BLSMSK: mask up to lowest set bit */
        uint64_t blsmsk_res;
        __asm__("blsmskq %1, %0" : "=r"(blsmsk_res) : "rm"(v));
        CHECK(blsmsk_res == 0x0F, "BMI1 BLSMSK generates mask (0x18 -> 0x0F)");

        /* ANDN: (~a) & b */
        uint64_t andn_res;
        __asm__("andnq %1, %2, %0" : "=r"(andn_res) : "rm"(0xFFULL), "r"(0x0FULL));
        CHECK(andn_res == 0xF0, "BMI1 ANDN computes (~0x0F) & 0xFF == 0xF0");
    } else {
        printf("[SKIP] BMI1 instructions not supported by CPU\n");
    }

    /* BMI2 */
    if (has_bmi2) {
        /* BZHI: clear high bits above index 8 */
        uint64_t v = 0x12345678ULL;
        uint64_t bzhi_res;
        __asm__("bzhiq %2, %1, %0" : "=r"(bzhi_res) : "rm"(v), "r"(8ULL));
        CHECK(bzhi_res == 0x78, "BMI2 BZHI masks bits above index 8");

        /* MULX: 64x64 -> 128 multiply without touching flags */
        uint64_t lo, hi;
        __asm__("mulxq %2, %0, %1" : "=r"(lo), "=r"(hi) : "rm"(0x100000000ULL), "d"(0x200000000ULL));
        CHECK(lo == 0 && hi == 2, "BMI2 MULX computes 0x100000000 * 0x200000000 == 2 << 64");
    } else {
        printf("[SKIP] BMI2 instructions not supported by CPU\n");
    }
}

static void test_crypto_and_rng(void)
{
    printf("\n--- Hardware RNG & Crypto Support ---\n");

    uint32_t a = 0, b = 0, c = 0, d = 0;
    cpuid(1, 0, &a, &b, &c, &d);
    bool has_rdrand = (c & (1u << 30)) != 0;

    cpuid(7, 0, &a, &b, &c, &d);
    bool has_rdseed = (b & (1u << 18)) != 0;

    if (has_rdrand) {
        uint64_t r1 = 0, r2 = 0;
        unsigned char ok1 = 0, ok2 = 0;
        __asm__ volatile("rdrand %0; setc %1" : "=r"(r1), "=qm"(ok1));
        __asm__ volatile("rdrand %0; setc %1" : "=r"(r2), "=qm"(ok2));

        CHECK(ok1 && ok2, "RDRAND executes and sets carry flag");
        CHECK(r1 != r2, "Consecutive RDRAND reads yield distinct random words");
        printf("       r1=0x%016lx r2=0x%016lx\n", r1, r2);
    } else {
        printf("[SKIP] RDRAND not supported by CPU\n");
    }

    if (has_rdseed) {
        uint64_t seed = 0;
        unsigned char ok = 0;
        __asm__ volatile("rdseed %0; setc %1" : "=r"(seed), "=qm"(ok));
        CHECK(ok, "RDSEED executes and sets carry flag");
        printf("       seed=0x%016lx\n", seed);
    } else {
        printf("[SKIP] RDSEED not supported by CPU\n");
    }
}

static void test_constant_time_and_crc(void)
{
    printf("\n--- Constant-Time Primitives & Hardware CRC32C ---\n");

    /* 1. explicit_bzero */
    char secret[64];
    memset(secret, 0x5A, sizeof(secret));
    explicit_bzero(secret, sizeof(secret));
    int zero_ok = 1;
    for (size_t i = 0; i < sizeof(secret); i++) {
        if (secret[i] != 0) { zero_ok = 0; break; }
    }
    CHECK(zero_ok, "explicit_bzero overwrites memory completely with zeros");

    /* 2. timingsafe_bcmp */
    char t1[32], t2[32];
    memset(t1, 'K', sizeof(t1));
    memset(t2, 'K', sizeof(t2));
    CHECK(timingsafe_bcmp(t1, t2, sizeof(t1)) == 0, "timingsafe_bcmp returns 0 for identical buffers");

    t2[0] = 'X';
    CHECK(timingsafe_bcmp(t1, t2, sizeof(t1)) != 0, "timingsafe_bcmp detects mismatch at first byte");
    t2[0] = 'K';
    t2[7] = 'X';
    CHECK(timingsafe_bcmp(t1, t2, sizeof(t1)) != 0, "timingsafe_bcmp detects mismatch at boundary byte");
    t2[7] = 'K';
    t2[31] = 'X';
    CHECK(timingsafe_bcmp(t1, t2, sizeof(t1)) != 0, "timingsafe_bcmp detects mismatch at final byte");

    /* 3. timingsafe_memcmp */
    CHECK(timingsafe_memcmp("alpha", "alpha", 5) == 0, "timingsafe_memcmp equal strings return 0");
    CHECK(timingsafe_memcmp("alpha", "alphz", 5) < 0, "timingsafe_memcmp correctly orders smaller string");
    CHECK(timingsafe_memcmp("alphz", "alpha", 5) > 0, "timingsafe_memcmp correctly orders larger string");

    /* 4. crc32c */
    uint32_t c = crc32c(0xFFFFFFFFu, "123456789", 9) ^ 0xFFFFFFFFu;
    CHECK(c == 0xE3069283u, "CRC-32C Castagnoli standard test vector matches 0xE3069283");

    /* 5. arc4random dual-source entropy */
    uint32_t rnd1 = arc4random();
    uint32_t rnd2 = arc4random();
    CHECK(rnd1 != 0 || rnd2 != 0, "arc4random produces nonzero output");
    CHECK(rnd1 != rnd2, "arc4random consecutive outputs differ");
}

int main(void)
{
    printf("====================================================\n");
    printf(" AzamiOS Hardware Instructions & Scanners Test Suite\n");
    printf("====================================================\n\n");

    test_string_simd_scanners();
    test_bitops();
    test_crypto_and_rng();
    test_constant_time_and_crc();

    printf("\n====================================================\n");
    if (g_fail == 0) {
        printf(" ALL HARDWARE INSTRUCTION TESTS PASSED!\n");
    } else {
        printf(" %d TEST(S) FAILED!\n", g_fail);
    }
    printf("====================================================\n");

    return g_fail ? 1 : 0;
}
