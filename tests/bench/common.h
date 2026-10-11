#ifndef AZAMI_BENCH_COMMON_H
#define AZAMI_BENCH_COMMON_H

#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static inline void bench_die(const char *operation)
{
    perror(operation);
    exit(1);
}

static inline uint64_t bench_number(const char *text, uint64_t maximum)
{
    char *end;
    errno = 0;
    if (!text[0] || text[0] < '0' || text[0] > '9') {
        fprintf(stderr, "invalid positive integer: %s\n", text);
        exit(2);
    }
    unsigned long long value = strtoull(text, &end, 10);
    if (errno || *end || !value || value > maximum) {
        fprintf(stderr, "integer out of range: %s\n", text);
        exit(2);
    }
    return (uint64_t)value;
}

static inline uint64_t bench_count(int argc, char **argv, uint64_t fallback,
                                  uint64_t maximum)
{
    if (argc > 2) {
        fprintf(stderr, "usage: %s [positive count]\n", argv[0]);
        exit(2);
    }
    return argc == 2 ? bench_number(argv[1], maximum) : fallback;
}

static inline uint64_t bench_timespec_ns(struct timespec ts)
{
    if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000L ||
        (uint64_t)ts.tv_sec > (UINT64_MAX - (uint64_t)ts.tv_nsec) / 1000000000) {
        fprintf(stderr, "invalid or overflowing monotonic timestamp\n");
        exit(1);
    }
    return (uint64_t)ts.tv_sec * 1000000000 + (uint64_t)ts.tv_nsec;
}

static inline uint64_t bench_now(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) bench_die("clock_gettime");
    return bench_timespec_ns(ts);
}

static inline uint64_t bench_resolution(void)
{
    struct timespec ts;
    if (clock_getres(CLOCK_MONOTONIC, &ts)) bench_die("clock_getres");
    uint64_t value = bench_timespec_ns(ts);
    if (!value) {
        fprintf(stderr, "zero monotonic timer resolution\n");
        exit(1);
    }
    return value;
}

static inline uint64_t bench_elapsed(uint64_t start)
{
    uint64_t end = bench_now();
    if (end <= start) {
        fprintf(stderr, "nonpositive benchmark duration\n");
        exit(1);
    }
    return end - start;
}

static inline int bench_wait(pid_t child)
{
    int status;
    pid_t result;
    do { result = waitpid(child, &status, 0); } while (result < 0 && errno == EINTR);
    if (result != child) return -1;
    if (!WIFEXITED(status) || WEXITSTATUS(status)) {
        fprintf(stderr, "child %ld failed (wait status %#x)\n", (long)child, status);
        return -1;
    }
    return 0;
}

/* One JSON record per measurement, with integer inputs and unrounded duration.
 * The host recomputes rates from these values instead of parsing display text. */
static inline void bench_result(const char *name, const char *variant,
                                uint64_t count, uint64_t warmup, uint64_t ns,
                                uint64_t resolution, uint64_t checksum)
{
    printf("AZBENCH {\"type\":\"sample\",\"benchmark\":\"%s\","
           "\"variant\":\"%s\",\"iterations\":%" PRIu64 ","
           "\"warmup_iterations\":%" PRIu64 ",\"elapsed_ns\":%" PRIu64 ","
           "\"timer_resolution_ns\":%" PRIu64 ",\"checksum\":%" PRIu64 "}\n",
           name, variant, count, warmup, ns, resolution, checksum);
}
#endif
