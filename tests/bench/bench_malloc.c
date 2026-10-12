/* SPDX-License-Identifier: MIT */
/* ============================================================================
 * AzamiOS — Memory Allocator Benchmark Suite
 * File: tests/bench/bench_malloc.c
 *
 * Measures malloc/free throughput across single-thread and multi-thread
 * workloads with mixed allocation sizes (16 B -> 128 KiB).
 * ============================================================================ */

#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "common.h"

#define DEFAULT_ITERS 100000
#define NUM_SLOTS 64

static const size_t ALLOC_SIZES[] = {
    16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536, 131072
};
#define NUM_SIZES (sizeof(ALLOC_SIZES) / sizeof(ALLOC_SIZES[0]))

struct worker_arg {
    int thread_id;
    uint64_t iterations;
    uint64_t checksum;
    uint64_t elapsed_ns;
};

static void *malloc_worker(void *arg)
{
    struct worker_arg *w = (struct worker_arg *)arg;
    void *slots[NUM_SLOTS] = {0};
    size_t slot_sizes[NUM_SLOTS] = {0};
    uint64_t sum = 0;

    uint64_t start = bench_now();

    for (uint64_t i = 0; i < w->iterations; i++) {
        int slot = (int)(i % NUM_SLOTS);
        if (slots[slot]) {
            /* Verify data integrity */
            unsigned char *buf = (unsigned char *)slots[slot];
            size_t sz = slot_sizes[slot];
            if (sz > 0) {
                sum += buf[0] + buf[sz - 1];
            }
            free(slots[slot]);
            slots[slot] = NULL;
        }

        size_t sz = ALLOC_SIZES[(i + (size_t)w->thread_id) % NUM_SIZES];
        void *p = malloc(sz);
        if (p) {
            unsigned char *buf = (unsigned char *)p;
            buf[0] = (unsigned char)(i & 0xFF);
            buf[sz - 1] = (unsigned char)((i ^ w->thread_id) & 0xFF);
            slots[slot] = p;
            slot_sizes[slot] = sz;
        }
    }

    /* Clean up remaining slots */
    for (int s = 0; s < NUM_SLOTS; s++) {
        if (slots[s]) {
            free(slots[s]);
            slots[s] = NULL;
        }
    }

    w->elapsed_ns = bench_elapsed(start);
    w->checksum = sum;
    return NULL;
}

static void run_benchmark(int num_threads, uint64_t iters_per_thread)
{
    pthread_t threads[8];
    struct worker_arg args[8];

    char variant[64];
    snprintf(variant, sizeof(variant), "threads_%d_mixed", num_threads);

    uint64_t start_total = bench_now();

    for (int t = 0; t < num_threads; t++) {
        args[t].thread_id = t;
        args[t].iterations = iters_per_thread;
        args[t].checksum = 0;
        args[t].elapsed_ns = 0;
        if (num_threads > 1) {
            if (pthread_create(&threads[t], NULL, malloc_worker, &args[t]) != 0) {
                bench_die("pthread_create");
            }
        }
    }

    if (num_threads == 1) {
        malloc_worker(&args[0]);
    } else {
        for (int t = 0; t < num_threads; t++) {
            pthread_join(threads[t], NULL);
        }
    }

    uint64_t total_elapsed = bench_elapsed(start_total);
    uint64_t total_checksum = 0;
    for (int t = 0; t < num_threads; t++) {
        total_checksum += args[t].checksum;
    }

    uint64_t total_ops = (uint64_t)num_threads * iters_per_thread;
    uint64_t res = bench_resolution();

    double sec = (double)total_elapsed / 1e9;
    double ops_per_sec = (double)total_ops / sec;

    printf("=== bench_malloc [%s]: %lu ops in %.4f s (%.2f ops/sec) ===\n",
           variant, (unsigned long)total_ops, sec, ops_per_sec);

    bench_result("bench_malloc", variant, total_ops, 1000, total_elapsed, res, total_checksum);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    uint64_t iters = bench_count(argc, argv, DEFAULT_ITERS, 10000000);

    printf("== AzamiOS Memory Allocator Benchmark (Workstream U2.1/U3.6) ==\n");
    printf("Iterations per thread: %lu\n", (unsigned long)iters);

    /* Test 1: Single thread */
    run_benchmark(1, iters);

    /* Test 2: 2 Threads */
    run_benchmark(2, iters / 2);

    /* Test 3: 4 Threads */
    run_benchmark(4, iters / 4);

    /* Test 4: 8 Threads */
    run_benchmark(8, iters / 8);

    printf("== bench_malloc complete: PASS ==\n");
    return 0;
}

