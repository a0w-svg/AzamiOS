#include "common.h"
#include <sys/syscall.h>

#if !defined(__x86_64__)
#error This raw Linux syscall benchmark requires x86-64.
#endif

static inline long raw_getpid(void)
{
    long result;
    __asm__ volatile ("syscall" : "=a"(result) : "a"(SYS_getpid)
                      : "rcx", "r11", "memory", "cc");
    return result;
}

int main(int argc, char **argv)
{
    uint64_t count = bench_count(argc, argv, 10000000, 1000000000);
    uint64_t warmup = count < 10000 ? count : 10000;
    uint64_t resolution = bench_resolution(), checksum = 0;
    long expected = getpid();
    if (expected <= 0) bench_die("getpid");
    for (uint64_t i = 0; i < warmup; i++) {
        if (raw_getpid() != expected) return 1;
    }
    uint64_t start = bench_now();
    for (uint64_t i = 0; i < count; i++) {
        long result = raw_getpid();
        if (result != expected) {
            fprintf(stderr, "raw getpid returned %ld, expected %ld\n", result, expected);
            return 1;
        }
        checksum += (uint64_t)result;
    }
    uint64_t ns = bench_elapsed(start);
    bench_result("syscall", "getpid", count, warmup, ns, resolution, checksum);
    return 0;
}
