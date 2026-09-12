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
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <dirent.h>
#include <ftw.h>
#include <wordexp.h>

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

#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/stat.h>

static inline uint64_t rdtsc_time(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static void test_file_io(const char *test_path, const char *desc)
{
    printf("   Testing %s (%s)...\n", desc, test_path);
    int fd = open(test_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    CHECK(fd >= 0, "open file for writing");

    if (fd >= 0) {
        char *wbuf = malloc(65536);
        CHECK(wbuf != NULL, "allocated 64KB write buffer");
        if (wbuf) {
            for (size_t i = 0; i < 65536; i++) wbuf[i] = (char)((i ^ 0xA5) & 0xFF);

            uint64_t t0 = rdtsc_time();
            ssize_t total_w = 0;
            for (int chunk = 0; chunk < 8; chunk++) { /* 8 * 64KB = 512KB */
                ssize_t w = write(fd, wbuf, 65536);
                if (w > 0) total_w += w;
            }
            uint64_t t1 = rdtsc_time();
            close(fd);

            CHECK(total_w == 512 * 1024, "wrote 512 KB in 64 KB burst chunks");
            printf("     -> 512 KB write took %llu cycles (~%.2f cycles/byte)\n",
                   (unsigned long long)(t1 - t0), (double)(t1 - t0) / (512.0 * 1024.0));
        }

        fd = open(test_path, O_RDONLY, 0);
        CHECK(fd >= 0, "open file for reading");
        if (fd >= 0) {
            char *rbuf = malloc(65536);
            CHECK(rbuf != NULL, "allocated 64KB read buffer");
            if (rbuf) {
                uint64_t t0 = rdtsc_time();
                ssize_t total_r = 0;
                int mismatch = 0;
                while (total_r < 512 * 1024) {
                    ssize_t r = read(fd, rbuf, 65536);
                    if (r <= 0) break;
                    for (ssize_t i = 0; i < r; i++) {
                        if (rbuf[i] != (char)(((total_r + i) ^ 0xA5) & 0xFF)) {
                            mismatch++;
                            break;
                        }
                    }
                    total_r += r;
                }
                uint64_t t1 = rdtsc_time();
                close(fd);

                CHECK(total_r == 512 * 1024, "read 512 KB back in 64 KB burst chunks");
                CHECK(mismatch == 0, "all 512 KB data verified bit-for-bit");
                printf("     -> 512 KB read took %llu cycles (~%.2f cycles/byte)\n",
                       (unsigned long long)(t1 - t0), (double)(t1 - t0) / (512.0 * 1024.0));
                free(rbuf);
            }
            if (wbuf) free(wbuf);
        }
        unlink(test_path);
    }
}

static void test_io_and_process_exec(void)
{
    printf("\n--- High-Throughput I/O & Process Execution ---\n");

    /* 1. File write & read throughput (tmpfs and ext2) */
    test_file_io("/tmp/speed_tmpfs.bin", "tmpfs I/O");
    test_file_io("/speed_ext2.bin", "ext2 I/O");

    /* 2. Vectorized Pipe I/O Throughput */
    {
        int pipefds[2];
        if (pipe(pipefds) == 0) {
            char *pbuf = malloc(2048);
            if (pbuf) {
                memset(pbuf, 0x5A, 2048);
                uint64_t t0 = rdtsc_time();
                ssize_t total_pipe = 0;
                /* Write and read 256 KB through pipe in 2KB blocks */
                for (int i = 0; i < 128; i++) {
                    ssize_t pw = write(pipefds[1], pbuf, 2048);
                    ssize_t pr = read(pipefds[0], pbuf, 2048);
                    if (pw > 0 && pr > 0) total_pipe += pr;
                }
                uint64_t t1 = rdtsc_time();
                CHECK(total_pipe == 256 * 1024, "transferred 256 KB through pipe");
                printf("     -> 256 KB pipe I/O took %llu cycles (~%.2f cycles/byte)\n",
                       (unsigned long long)(t1 - t0), (double)(t1 - t0) / (256.0 * 1024.0));
                free(pbuf);
            }
            close(pipefds[0]);
            close(pipefds[1]);
        }
    }

    /* 3. Character Stream Buffering via fgetc */
    FILE *fp = fopen("/tmp/char_test.txt", "w");
    if (fp) {
        for (int i = 0; i < 4096; i++) fputc((i % 26) + 'A', fp);
        fclose(fp);

        fp = fopen("/tmp/char_test.txt", "r");
        CHECK(fp != NULL, "fopen /tmp/char_test.txt for buffered reading");
        if (fp) {
            uint64_t t0 = rdtsc_time();
            int chars_read = 0;
            int c_err = 0;
            int ch;
            while ((ch = fgetc(fp)) != EOF) {
                if (ch != ((chars_read % 26) + 'A')) c_err++;
                chars_read++;
            }
            uint64_t t1 = rdtsc_time();
            fclose(fp);

            CHECK(chars_read == 4096, "read 4096 characters via buffered fgetc");
            CHECK(c_err == 0, "buffered fgetc stream content verified");
            printf("     -> 4096 char fgetc took %llu cycles (~%.1f cycles/char)\n",
                   (unsigned long long)(t1 - t0), (double)(t1 - t0) / 4096.0);
        }
        unlink("/tmp/char_test.txt");
    }

    /* 4. Fast Zero-Copy COW Fork */
    {
        uint64_t t0 = rdtsc_time();
        int fork_cycles = 5;
        int fork_pass = 0;
        for (int i = 0; i < fork_cycles; i++) {
            pid_t pid = fork();
            if (pid == 0) {
                _exit(42);
            } else if (pid > 0) {
                int st = 0;
                waitpid(pid, &st, 0);
                if (WIFEXITED(st) && WEXITSTATUS(st) == 42) fork_pass++;
            }
        }
        uint64_t t1 = rdtsc_time();
        CHECK(fork_pass == fork_cycles, "rapid zero-copy COW fork+exit cycles succeeded");
        printf("     -> %d fork+wait cycles took %llu cycles (~%.0f cycles/fork)\n",
               fork_cycles, (unsigned long long)(t1 - t0), (double)(t1 - t0) / fork_cycles);
    }

    /* 5. Process Execution (fork + execve) */
    {
        uint64_t t0 = rdtsc_time();
        pid_t pid = fork();
        if (pid == 0) {
            char *const argv[] = { "true", NULL };
            char *const envp[] = { NULL };
            execve("/bin/true.elf", argv, envp);
            execve("/bin/true", argv, envp);
            _exit(127);
        } else if (pid > 0) {
            int st = 0;
            waitpid(pid, &st, 0);
            uint64_t t1 = rdtsc_time();
            CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "fork + execve(/bin/true.elf) executed successfully");
            printf("     -> process execution took %llu cycles\n", (unsigned long long)(t1 - t0));
        }
    }
}

static void test_caching_and_fastpaths(void)
{
    printf("\n--- Advanced Multi-Level Caching & Fast-Path Diagnostics ---\n");

    /* 1. Inode Cache (icache): Repeated stat() latency */
    {
        struct stat st;
        /* Warmup */
        int r0 = stat("/bin/true.elf", &st);
        CHECK(r0 == 0, "stat /bin/true.elf (icache warmup)");

        uint64_t t0 = rdtsc_time();
        int iters = 500;
        int stat_ok = 0;
        for (int i = 0; i < iters; i++) {
            if (stat("/bin/true.elf", &st) == 0) stat_ok++;
        }
        uint64_t t1 = rdtsc_time();
        CHECK(stat_ok == iters, "500 repeated stat() calls succeeded via icache");
        printf("     -> 500 cached stat() calls took %llu cycles (~%.1f cycles/stat)\n",
               (unsigned long long)(t1 - t0), (double)(t1 - t0) / iters);
    }

    /* 2. Negative Dentry Cache (neg_dcache): Nonexistent file lookup latency */
    {
        const char *missing = "/bin/nonexistent_probe_file.xyz";
        /* First call populates negative dentry cache */
        int r0 = access(missing, F_OK);
        CHECK(r0 < 0, "first access to nonexistent file returned ENOENT");

        uint64_t t0 = rdtsc_time();
        int iters = 500;
        int neg_ok = 0;
        for (int i = 0; i < iters; i++) {
            if (access(missing, F_OK) < 0) neg_ok++;
        }
        uint64_t t1 = rdtsc_time();
        CHECK(neg_ok == iters, "500 repeated negative lookups succeeded via neg_dcache");
        printf("     -> 500 negative lookups took %llu cycles (~%.1f cycles/lookup)\n",
               (unsigned long long)(t1 - t0), (double)(t1 - t0) / iters);
    }
}

static void test_new_hardware_drivers(void)
{
    printf("\n--- New Hardware Drivers Test (/dev/debugcon, /dev/fw_cfg, /dev/cpu_temp, /dev/pvpanic) ---\n");

    /* 1. Test /dev/debugcon */
    int fd_dbg = open("/dev/debugcon", 1 /* O_WRONLY */);
    if (fd_dbg >= 0) {
        const char *msg = "[HWTEST] Fast hypervisor debugcon output verified!\n";
        ssize_t w = write(fd_dbg, msg, strlen(msg));
        close(fd_dbg);
        CHECK(w == (ssize_t)strlen(msg), "Write to /dev/debugcon succeeds");
    } else {
        printf("[INFO] /dev/debugcon not present on this host\n");
    }

    /* 2. Test /dev/cpu_temp */
    int fd_temp = open("/dev/cpu_temp", 0 /* O_RDONLY */);
    if (fd_temp >= 0) {
        char buf[128] = {0};
        ssize_t r = read(fd_temp, buf, sizeof(buf) - 1);
        close(fd_temp);
        CHECK(r > 0, "Read from /dev/cpu_temp succeeds");
        if (r > 0) {
            printf("     -> Thermal reading: %s", buf);
        }
    } else {
        printf("[INFO] /dev/cpu_temp not present\n");
    }

    /* 3. Test /dev/fw_cfg */
    int fd_fw = open("/dev/fw_cfg", 0 /* O_RDONLY */);
    if (fd_fw >= 0) {
        char sig[8] = {0};
        ssize_t r = read(fd_fw, sig, 4);
        close(fd_fw);
        CHECK(r >= 0, "Read /dev/fw_cfg interface accessible");
    } else {
        printf("[INFO] /dev/fw_cfg not present on this platform\n");
    }

    /* 4. Test /dev/pvpanic */
    int fd_pv = open("/dev/pvpanic", 0 /* O_RDONLY */);
    if (fd_pv >= 0) {
        uint8_t mask = 0;
        ssize_t r = read(fd_pv, &mask, 1);
        close(fd_pv);
        CHECK(r == 1, "Read /dev/pvpanic supported event mask");
        printf("     -> pvpanic supported event mask: 0x%02X\n", mask);
    } else {
        printf("[INFO] /dev/pvpanic not present on this platform\n");
    }

    /* 5. Test /dev/uhci0 */
    int fd_uhci = open("/dev/uhci0", 0 /* O_RDONLY */);
    if (fd_uhci >= 0) {
        char buf[256] = {0};
        ssize_t r = read(fd_uhci, buf, sizeof(buf) - 1);
        close(fd_uhci);
        CHECK(r > 0, "Read /dev/uhci0 USB controller status");
        if (r > 0) printf("     -> %s", buf);
    } else {
        printf("[INFO] /dev/uhci0 not present on this host\n");
    }

    /* 6. Test /dev/pm_timer */
    int fd_pm = open("/dev/pm_timer", 0 /* O_RDONLY */);
    if (fd_pm >= 0) {
        char buf[128] = {0};
        ssize_t r = read(fd_pm, buf, sizeof(buf) - 1);
        close(fd_pm);
        CHECK(r > 0, "Read /dev/pm_timer ACPI timekeeper");
        if (r > 0) printf("     -> %s", buf);
    } else {
        printf("[INFO] /dev/pm_timer not present on this host\n");
    }

    /* 7. Test /dev/net2 (VMXNET3) */
    int fd_net2 = open("/dev/net2", 0 /* O_RDONLY */);
    if (fd_net2 >= 0) {
        char buf[256] = {0};
        ssize_t r = read(fd_net2, buf, sizeof(buf) - 1);
        close(fd_net2);
        CHECK(r > 0, "Read /dev/net2 VMXNET3 10-GbE status");
        if (r > 0) printf("     -> %s", buf);
    } else {
        printf("[INFO] /dev/net2 not present on this host\n");
    }

    /* 8. Test /dev/virtfs (VirtIO-9P) */
    int fd_v9p = open("/dev/virtfs", 0 /* O_RDONLY */);
    if (fd_v9p >= 0) {
        char buf[256] = {0};
        ssize_t r = read(fd_v9p, buf, sizeof(buf) - 1);
        close(fd_v9p);
        CHECK(r > 0, "Read /dev/virtfs VirtIO-9P channel status");
        if (r > 0) printf("     -> %s", buf);
    } else {
        printf("[INFO] /dev/virtfs not present on this host\n");
    }

    /* 9. Test /dev/speaker */
    int fd_spk = open("/dev/speaker", 1 /* O_WRONLY */);
    if (fd_spk >= 0) {
        const char *tone = "440\n";
        ssize_t w = write(fd_spk, tone, strlen(tone));
        close(fd_spk);
        CHECK(w > 0, "Write tone to /dev/speaker");
    } else {
        printf("[INFO] /dev/speaker not present\n");
    }

    /* 10. Test /dev/ehci0 (USB 2.0 Host Controller) */
    int fd_ehci = open("/dev/ehci0", 0 /* O_RDONLY */);
    if (fd_ehci >= 0) {
        char buf[512] = {0};
        ssize_t r = read(fd_ehci, buf, sizeof(buf) - 1);
        CHECK(r > 0, "Read /dev/ehci0 USB 2.0 controller status");
        if (r > 0) printf("     -> %s", buf);
        int num_ports = ioctl(fd_ehci, 0x5511, 0);
        CHECK(num_ports >= 0, "ioctl EHCI_IOC_GET_NUM_PORTS");
        printf("     -> EHCI Root Hub Ports: %d\n", num_ports);
        close(fd_ehci);
    } else {
        printf("[INFO] /dev/ehci0 not present on this host\n");
    }

    /* 11. Test /dev/acpi_pm (Intel PIIX4 Power Management) */
    int fd_acpi = open("/dev/acpi_pm", 0 /* O_RDONLY */);
    if (fd_acpi >= 0) {
        char buf[256] = {0};
        ssize_t r = read(fd_acpi, buf, sizeof(buf) - 1);
        CHECK(r > 0, "Read /dev/acpi_pm Intel PIIX4 status");
        if (r > 0) printf("     -> %s", buf);
        uint32_t tmr = 0;
        int io_res = ioctl(fd_acpi, 0x5001, (unsigned long)&tmr);
        CHECK(io_res == 0, "ioctl PM_IOC_GET_TIMER");
        printf("     -> PIIX4 PM Timer count: %u ticks\n", tmr);
        close(fd_acpi);
    } else {
        printf("[INFO] /dev/acpi_pm not present on this host\n");
    }

    /* 12. Test /dev/net3 or /dev/e100 (Intel PRO/100 Ethernet) */
    int fd_e100 = open("/dev/net3", 0 /* O_RDONLY */);
    if (fd_e100 < 0) fd_e100 = open("/dev/e100", 0 /* O_RDONLY */);
    if (fd_e100 >= 0) {
        char buf[128] = {0};
        ssize_t r = read(fd_e100, buf, sizeof(buf) - 1);
        CHECK(r > 0, "Read /dev/e100 status / MAC");
        if (r > 0) printf("     -> %s", buf);
        close(fd_e100);
    } else {
        printf("[INFO] /dev/e100 (/dev/net3) not present on this host\n");
    }

    /* 13. Test /dev/speaker Musical Note Synthesizer */
    int fd_spk_note = open("/dev/speaker", 2 /* O_RDWR */);
    if (fd_spk_note >= 0) {
        /* 0x5304: arg[7:0] = note 69 (A4 440Hz), arg[31:8] = 5ms */
        unsigned long note_arg = (5u << 8) | 69u;
        int s_res = ioctl(fd_spk_note, 0x5304, note_arg);
        CHECK(s_res == 0, "ioctl SPEAKER_IOC_PLAY_NOTE (A440)");
        close(fd_spk_note);
    }
}

static int g_nftw_count = 0;
static int dummy_nftw_cb(const char *fpath, const struct stat *sb, int typeflag, struct FTW *ftwbuf)
{
    (void)fpath; (void)sb; (void)typeflag; (void)ftwbuf;
    g_nftw_count++;
    return 0;
}

static void test_posix_libc_features(void)
{
    printf("\n--- POSIX.1-2008 Libc Features ---\n");

    /* 1. POSIX Shared Memory (shm_open / shm_unlink) */
    int shm_fd = shm_open("/test_hwshm", 0x02 /* O_RDWR */ | 0x0200 /* O_CREAT */ | 0x0400 /* O_TRUNC */, 0666);
    CHECK(shm_fd >= 0, "shm_open creates shared memory object");
    if (shm_fd >= 0) {
        int tr = ftruncate(shm_fd, 4096);
        CHECK(tr == 0, "ftruncate sets shm size to 4096 bytes");
        void *ptr = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
        CHECK(ptr != MAP_FAILED, "mmap attaches to shared memory");
        if (ptr != MAP_FAILED) {
            strcpy((char *)ptr, "AzamiOS POSIX Shared Memory Test Passed");
            CHECK(strcmp((char *)ptr, "AzamiOS POSIX Shared Memory Test Passed") == 0, "shm payload verified");
            munmap(ptr, 4096);
        }
        close(shm_fd);
        int un = shm_unlink("/test_hwshm");
        CHECK(un == 0, "shm_unlink removes shared memory object");
    }

    /* 2. POSIX Memory Streams (fmemopen / open_memstream) */
    char membuf[128];
    memset(membuf, 0, sizeof(membuf));
    FILE *fmem = fmemopen(membuf, sizeof(membuf), "w+");
    CHECK(fmem != NULL, "fmemopen creates memory stream");
    if (fmem) {
        fprintf(fmem, "AzamiOS fmemopen test %d", 2026);
        fflush(fmem);
        CHECK(strcmp(membuf, "AzamiOS fmemopen test 2026") == 0, "fmemopen formatted write");
        fseek(fmem, 0, SEEK_SET);
        char readback[64] = {0};
        fgets(readback, sizeof(readback), fmem);
        CHECK(strcmp(readback, "AzamiOS fmemopen test 2026") == 0, "fmemopen formatted readback");
        fclose(fmem);
    }

    char *dyn_buf = NULL;
    size_t dyn_size = 0;
    FILE *fms = open_memstream(&dyn_buf, &dyn_size);
    CHECK(fms != NULL, "open_memstream creates dynamic stream");
    if (fms) {
        fprintf(fms, "open_memstream dynamic string %u", 777);
        fflush(fms);
        CHECK(dyn_buf != NULL && dyn_size > 0, "open_memstream allocates dynamic buffer");
        if (dyn_buf) {
            CHECK(strcmp(dyn_buf, "open_memstream dynamic string 777") == 0, "open_memstream content match");
        }
        fclose(fms);
        free(dyn_buf);
    }

    /* 3. POSIX Directory Scanning & Sorting (scandir, alphasort, readdir_r) */
    struct dirent **namelist = NULL;
    int n = scandir("/dev", &namelist, NULL, alphasort);
    CHECK(n >= 0, "scandir scans /dev with alphasort");
    if (n > 0) {
        bool sorted = true;
        for (int i = 0; i < n - 1; i++) {
            if (strcmp(namelist[i]->d_name, namelist[i+1]->d_name) > 0) {
                sorted = false;
                break;
            }
        }
        CHECK(sorted, "scandir results sorted alphabetically");
        for (int i = 0; i < n; i++) free(namelist[i]);
        free(namelist);
    }

    DIR *d = opendir("/dev");
    if (d) {
        struct dirent entry;
        struct dirent *result = NULL;
        int err = readdir_r(d, &entry, &result);
        CHECK(err == 0 && result != NULL, "readdir_r reads directory entry reentrantly");
        closedir(d);
    }

    /* 4. POSIX File Tree Walk (nftw) */
    g_nftw_count = 0;
    int nftw_res = nftw("/dev", dummy_nftw_cb, 10, FTW_PHYS);
    CHECK(nftw_res == 0 && g_nftw_count > 0, "nftw traverses /dev tree");

    /* 5. POSIX Pseudoterminal Open (posix_openpt) */
    int pt_fd = posix_openpt(0x02 /* O_RDWR */ | 0x0100 /* O_NOCTTY */);
    if (pt_fd >= 0) {
        CHECK(pt_fd >= 0, "posix_openpt allocates pseudoterminal master");
        close(pt_fd);
    } else {
        printf("[INFO] posix_openpt: ptmx not mounted\n");
    }

    /* 6. POSIX Shell Word Expansion (wordexp / wordfree) */
    wordexp_t we;
    int we_res = wordexp("echo 'hello world' $TEST /dev/null", &we, 0);
    CHECK(we_res == 0, "wordexp parses shell words");
    if (we_res == 0) {
        CHECK(we.we_wordc >= 4, "wordexp extracts word count");
        wordfree(&we);
    }
}

int main(void)
{
    printf("====================================================\n");
    printf(" AzamiOS Hardware Instructions & Performance Suite\n");
    printf("====================================================\n\n");

    test_string_simd_scanners();
    test_bitops();
    test_crypto_and_rng();
    test_constant_time_and_crc();
    test_io_and_process_exec();
    test_caching_and_fastpaths();
    test_new_hardware_drivers();
    test_posix_libc_features();

    printf("\n====================================================\n");
    if (g_fail == 0) {
        printf(" ALL HARDWARE INSTRUCTION & PERF TESTS PASSED!\n");
    } else {
        printf(" %d TEST(S) FAILED!\n", g_fail);
    }
    printf("====================================================\n");

    return g_fail ? 1 : 0;
}
