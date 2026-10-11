#include "common.h"
#include <sys/mman.h>

static uint64_t random_next(uint64_t *state)
{
    *state ^= *state << 13;
    *state ^= *state >> 7;
    *state ^= *state << 17;
    return *state;
}

static void fault_pages(size_t bytes, size_t page_size, int shuffled,
                         uint64_t resolution, int report)
{
    size_t pages = bytes / page_size;
    size_t *order = NULL;
    if (shuffled) {
        if (pages > SIZE_MAX / sizeof(*order)) exit(1);
        order = malloc(pages * sizeof(*order));
        if (!order) bench_die("page order malloc");
        for (size_t i = 0; i < pages; i++) order[i] = i;
        uint64_t state = UINT64_C(0x4d595df4d0f33173);
        for (size_t i = pages - 1; i > 0; i--) {
            size_t j = (size_t)(random_next(&state) % (i + 1));
            size_t temporary = order[i]; order[i] = order[j]; order[j] = temporary;
        }
    }
    uint64_t map_start = bench_now();
    volatile unsigned char *memory = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) { free(order); bench_die("mmap"); }
    uint64_t map_ns = bench_elapsed(map_start);
    uint64_t start = bench_now();
    for (size_t i = 0; i < pages; i++) {
        size_t page = shuffled ? order[i] : i;
        memory[page * page_size] = (unsigned char)(page % 251 + 1);
    }
    uint64_t touch_ns = bench_elapsed(start), checksum = 0;
    int failed = 0;
    for (size_t i = 0; i < pages; i++) {
        unsigned char value = memory[i * page_size];
        if (value != (unsigned char)(i % 251 + 1) || memory[i * page_size + 1] ||
            memory[(i + 1) * page_size - 1]) failed = 1;
        checksum += value;
    }
    if (munmap((void *)memory, bytes)) bench_die("munmap");
    free(order);
    if (failed) { fprintf(stderr, "anonymous page content mismatch\n"); exit(1); }
    if (report) {
        /* No hardware fault count is inferred: units are logical pages touched. */
        printf("AZBENCH {\"type\":\"sample\",\"benchmark\":\"mmap\","
               "\"variant\":\"%s\",\"iterations\":%zu,\"warmup_iterations\":16,"
               "\"elapsed_ns\":%" PRIu64 ",\"timer_resolution_ns\":%" PRIu64 ","
               "\"mmap_elapsed_ns\":%" PRIu64 ",\"touch_elapsed_ns\":%" PRIu64 ","
               "\"checksum\":%" PRIu64 ",\"bytes\":%zu,\"page_size\":%zu,"
               "\"shuffle_seed\":\"4d595df4d0f33173\"}\n",
               shuffled ? "shuffled" : "sequential", pages, map_ns + touch_ns, resolution,
               map_ns, touch_ns,
               checksum, bytes, page_size);
    }
}

int main(int argc, char **argv)
{
    uint64_t mib = bench_count(argc, argv, 256, 65536);
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size < 2 || mib > SIZE_MAX / 1048576 ||
        (mib * 1048576) % (uint64_t)page_size) {
        fprintf(stderr, "invalid page size or mapping size\n");
        return 1;
    }
    uint64_t resolution = bench_resolution();
    fault_pages((size_t)page_size * 16, (size_t)page_size, 0, resolution, 0);
    fault_pages((size_t)mib * 1048576, (size_t)page_size, 0, resolution, 1);
    fault_pages((size_t)mib * 1048576, (size_t)page_size, 1, resolution, 1);
    return 0;
}
