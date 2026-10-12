/* End-to-end stock Linux syscall measurements, not isolated uaccess timings.
 * Each timed pair includes two seeks, one write, one read, and endpoint/guard
 * checks. Full-buffer validation and checksum calculation are outside timing. */
#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

enum { SIZE_COUNT = 6, REPETITIONS = 10, WARM_PAIRS = 4, GUARD_BYTES = 8 };
static const size_t sizes[SIZE_COUNT] = {16, 64, 256, 4096, 65536, 1048576};
static const unsigned char front_guard[GUARD_BYTES] = {0xb7,0x23,0xda,0x49,0x16,0xef,0x80,0x5c};
static const unsigned char back_guard[GUARD_BYTES] = {0x31,0xc4,0x08,0xfa,0x62,0x9d,0x47,0xe0};
static volatile sig_atomic_t stopped;
static unsigned passed;

struct buffers {
    int fd;
    size_t size;
    unsigned char *source_base, *destination_base;
    uint64_t expected_sum;
};

static void stop(int signo)
{
    (void)signo;
    stopped = 1;
}

static bool timestamp(uint64_t *value)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) || ts.tv_sec < 0 ||
        ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000L ||
        (uint64_t)ts.tv_sec > (UINT64_MAX - (uint64_t)ts.tv_nsec) / UINT64_C(1000000000))
        return false;
    *value = (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
    return true;
}

static bool fail(const char *operation, size_t size, int repetition, unsigned iteration)
{
    printf("COPYFAIL operation=%s size=%zu repetition=%d iteration=%u errno=%d timeout=%d\n",
           operation, size, repetition, iteration, errno, (int)stopped);
    return false;
}

static bool canaries(const struct buffers *b)
{
    return !memcmp(b->source_base, front_guard, GUARD_BYTES) &&
           !memcmp(b->destination_base, front_guard, GUARD_BYTES) &&
           !memcmp(b->source_base + GUARD_BYTES + b->size, back_guard, GUARD_BYTES) &&
           !memcmp(b->destination_base + GUARD_BYTES + b->size, back_guard, GUARD_BYTES);
}

static bool pair(struct buffers *b, int repetition, unsigned iteration)
{
    unsigned char *source = b->source_base + GUARD_BYTES;
    unsigned char *destination = b->destination_base + GUARD_BYTES;
    if (stopped) return fail("alarm", b->size, repetition, iteration);
    if (lseek(b->fd, 0, SEEK_SET) != 0) return fail("write-seek", b->size, repetition, iteration);
    if (write(b->fd, source, b->size) != (ssize_t)b->size)
        return fail("write-transfer", b->size, repetition, iteration);
    if (lseek(b->fd, 0, SEEK_SET) != 0) return fail("read-seek", b->size, repetition, iteration);
    if (read(b->fd, destination, b->size) != (ssize_t)b->size)
        return fail("read-transfer", b->size, repetition, iteration);
    if (destination[0] != source[0] || destination[b->size - 1] != source[b->size - 1])
        return fail("endpoint-data", b->size, repetition, iteration);
    if (!canaries(b)) return fail("canary", b->size, repetition, iteration);
    return true;
}

static bool run(void)
{
    struct buffers buffers[SIZE_COUNT] = {{0}};
    for (unsigned i = 0; i < SIZE_COUNT; i++) buffers[i].fd = -1;
    bool good = false;
    cpu_set_t available, pinned;
    CPU_ZERO(&available);
    if (sched_getaffinity(0, sizeof(available), &available))
        return fail("get-affinity", 0, -1, 0);
    int cpu = -1;
    for (int i = 0; i < CPU_SETSIZE; i++)
        if (CPU_ISSET(i, &available)) { cpu = i; break; }
    CPU_ZERO(&pinned);
    if (cpu < 0) return fail("empty-affinity", 0, -1, 0);
    CPU_SET(cpu, &pinned);
    if (sched_setaffinity(0, sizeof(pinned), &pinned) || sched_getcpu() != cpu)
        return fail("pin-affinity", 0, -1, 0);

    struct timespec resolution;
    if (clock_getres(CLOCK_MONOTONIC, &resolution) || resolution.tv_sec < 0 ||
        resolution.tv_nsec < 0 || resolution.tv_nsec >= 1000000000L ||
        (uint64_t)resolution.tv_sec >
        (UINT64_MAX - (uint64_t)resolution.tv_nsec) / UINT64_C(1000000000))
        return fail("timer-resolution", 0, -1, 0);
    uint64_t resolution_ns = (uint64_t)resolution.tv_sec * UINT64_C(1000000000) +
                             (uint64_t)resolution.tv_nsec;
    if (!resolution_ns) return fail("zero-timer-resolution", 0, -1, 0);
    printf("COPYENV {\"cases\":66,\"warmups\":1,\"repetitions\":10,\"cpu\":%d,\"timer_resolution_ns\":%" PRIu64 "}\n",
           cpu, resolution_ns);

    for (unsigned i = 0; i < SIZE_COUNT; i++) {
        struct buffers *b = &buffers[i];
        b->size = sizes[i];
        b->source_base = malloc(b->size + 2 * GUARD_BYTES);
        b->destination_base = malloc(b->size + 2 * GUARD_BYTES);
        b->fd = memfd_create("bench-usercopy", MFD_CLOEXEC);
        if (!b->source_base || !b->destination_base || b->fd < 0) {
            fail("allocate", b->size, -1, 0);
            goto out;
        }
        memcpy(b->source_base, front_guard, GUARD_BYTES);
        memcpy(b->destination_base, front_guard, GUARD_BYTES);
        memcpy(b->source_base + GUARD_BYTES + b->size, back_guard, GUARD_BYTES);
        memcpy(b->destination_base + GUARD_BYTES + b->size, back_guard, GUARD_BYTES);
        for (size_t n = 0; n < b->size; n++) {
            unsigned char value = (unsigned char)((n * 17 + b->size) % 251);
            b->source_base[GUARD_BYTES + n] = value;
            b->expected_sum += value;
        }
        /* Touch every destination byte before any timed operation. */
        memset(b->destination_base + GUARD_BYTES, 0, b->size);
    }

    uint64_t last_end = 0;
    for (int repetition = -1; repetition < REPETITIONS; repetition++) {
        for (unsigned i = 0; i < SIZE_COUNT; i++) {
            struct buffers *b = &buffers[i];
            size_t count = 4194304 / b->size;
            if (count > 4096) count = 4096;
            if (count < 4) count = 4;
            unsigned iterations = (unsigned)count;
            for (unsigned warm = 0; warm < WARM_PAIRS; warm++)
                if (!pair(b, repetition, warm)) goto out;
            uint64_t begin, end;
            if (stopped || !timestamp(&begin) || begin < last_end) {
                fail("timer-start", b->size, repetition, 0);
                goto out;
            }
            for (unsigned iteration = 0; iteration < iterations; iteration++)
                if (!pair(b, repetition, iteration)) goto out;
            if (stopped || !timestamp(&end) || end <= begin) {
                fail("timer-end", b->size, repetition, iterations);
                goto out;
            }
            last_end = end;
            unsigned char *source = b->source_base + GUARD_BYTES;
            unsigned char *destination = b->destination_base + GUARD_BYTES;
            if (memcmp(source, destination, b->size) || !canaries(b)) {
                fail("full-data", b->size, repetition, iterations);
                goto out;
            }
            uint64_t checksum = 0;
            for (size_t n = 0; n < b->size; n++) {
                unsigned char expected = (unsigned char)((n * 17 + b->size) % 251);
                if (source[n] != expected) {
                    fail("source-pattern", b->size, repetition, iterations);
                    goto out;
                }
                checksum += destination[n];
            }
            if (checksum != b->expected_sum || sched_getcpu() != cpu) {
                fail("checksum-or-cpu", b->size, repetition, iterations);
                goto out;
            }
            printf("COPYBENCH {\"size_bytes\":%zu,\"repetition\":%d,\"iterations\":%u,\"elapsed_ns\":%" PRIu64 ",\"checksum\":%" PRIu64 ",\"canary\":true}\n",
                   b->size, repetition, iterations, end - begin, checksum);
            passed++;
        }
    }
    good = true;
out:
    for (unsigned i = 0; i < SIZE_COUNT; i++) {
        if (buffers[i].fd >= 0) close(buffers[i].fd);
        free(buffers[i].source_base);
        free(buffers[i].destination_base);
    }
    return good;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = stop;
    sigemptyset(&action.sa_mask);
    bool good;
    if (sigaction(SIGALRM, &action, NULL)) good = fail("alarm-handler", 0, -1, 0);
    else {
        alarm(60);
        good = run();
        alarm(0);
    }
    printf("== probe complete: %u passed, %u failed ==\n", passed, good ? 0U : 1U);
    if (!good) return 1;
    if (getenv("USERCOPY_BENCH_EXIT")) return 0;
    for (;;) pause();
}
