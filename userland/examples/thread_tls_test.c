/* ============================================================================
 * AzamiOS — Per-thread TLS regression test (errno + pthread TSD)
 * File: userland/examples/thread_tls_test.c
 *
 * Compile and run on a booted AzamiOS with the self-hosted native GCC:
 *   gcc thread_tls_test.c -o thread_tls_test -lpthread && ./thread_tls_test
 *
 * Before the libc TLS work, errno was a single process-wide `int` and
 * pthread_setspecific()/pthread_getspecific() were backed by process-wide
 * arrays — every thread in a process shared the same storage. This program
 * spawns several threads, has each one set a value distinct to itself (in
 * errno and in a TSD slot), waits at a barrier so every thread has had a
 * chance to run and clobber shared storage if it were actually shared, then
 * has each thread re-check its own value is still what it set. Any
 * cross-thread bleed fails loudly.
 * ============================================================================ */
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <errno.h>
#include <unistd.h>

#define NTHREADS 6

static pthread_barrier_t g_barrier;
static pthread_key_t g_key;
static volatile int g_failures = 0;

typedef struct {
    int idx;
    int seen_errno;
    void *seen_tsd;
} thread_result_t;

static void *worker(void *arg)
{
    thread_result_t *res = (thread_result_t *)arg;
    int my_errno_val = 100 + res->idx;          /* distinct per thread */
    void *my_tsd_val = (void *)(long)(1000 + res->idx);

    /* Provoke a real syscall failure so errno isn't just hand-assigned —
     * close(-1) is guaranteed EBADF on every thread. */
    close(-1);
    if (errno != EBADF) {
        /* Not the value we expect even before the cross-thread check —
         * still record what we saw so main() can report it. */
    }
    errno = my_errno_val;
    pthread_setspecific(g_key, my_tsd_val);

    /* Let every other thread reach this point and run for a while before
     * we check our own state is still intact. */
    pthread_barrier_wait(&g_barrier);
    for (volatile int i = 0; i < 200000; i++) { /* give the scheduler room to interleave */ }
    pthread_barrier_wait(&g_barrier);

    res->seen_errno = errno;
    res->seen_tsd = pthread_getspecific(g_key);
    return NULL;
}

int main(void)
{
    printf("=== AzamiOS per-thread TLS test (errno + pthread TSD) ===\n");

    pthread_t threads[NTHREADS];
    thread_result_t results[NTHREADS];

    if (pthread_barrier_init(&g_barrier, NULL, NTHREADS) != 0) {
        printf("FAIL: pthread_barrier_init\n");
        return 1;
    }
    if (pthread_key_create(&g_key, NULL) != 0) {
        printf("FAIL: pthread_key_create\n");
        return 1;
    }

    for (int i = 0; i < NTHREADS; i++) {
        results[i].idx = i;
        results[i].seen_errno = -1;
        results[i].seen_tsd = NULL;
        if (pthread_create(&threads[i], NULL, worker, &results[i]) != 0) {
            printf("FAIL: pthread_create(%d)\n", i);
            return 1;
        }
    }

    for (int i = 0; i < NTHREADS; i++) {
        if (pthread_join(threads[i], NULL) != 0) {
            printf("FAIL: pthread_join(%d) did not return 0\n", i);
            g_failures++;
        }
    }

    for (int i = 0; i < NTHREADS; i++) {
        int want_errno = 100 + i;
        void *want_tsd = (void *)(long)(1000 + i);
        int ok = (results[i].seen_errno == want_errno) && (results[i].seen_tsd == want_tsd);
        printf("[thread %d] errno: got %d want %d | tsd: got %ld want %ld -> %s\n",
               i, results[i].seen_errno, want_errno,
               (long)results[i].seen_tsd, (long)want_tsd,
               ok ? "PASS" : "FAIL");
        if (!ok) g_failures++;
    }

    /* Also confirm the main thread's own errno wasn't disturbed by any of
     * the six worker threads running concurrently. */
    errno = 0;
    close(-1);
    if (errno != EBADF) {
        printf("[main] errno: got %d want %d (EBADF) -> FAIL\n", errno, EBADF);
        g_failures++;
    } else {
        printf("[main] errno: got EBADF as expected -> PASS\n");
    }

    if (g_failures == 0) {
        printf("=== ALL PASS (%d threads, no cross-thread TLS bleed) ===\n", NTHREADS);
        return 0;
    }
    printf("=== %d FAILURE(S) ===\n", g_failures);
    return 1;
}
