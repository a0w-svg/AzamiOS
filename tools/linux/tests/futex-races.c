#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define WAIT 0
#define WAKE 1
#define REQUEUE 3
#define PRIVATE 128

static int passed, failed;
static uint32_t words[32] __attribute__((aligned(256)));
static int timed;
static int waiter_done;
static void check(int good, const char *name)
{
    printf("  %s %s\n", good ? "ok  " : "FAIL", name);
    if (good) passed++; else failed++;
}
static long futex(uint32_t *word, int op, long count, void *arg, uint32_t *other)
{
    return syscall(SYS_futex, word, op | PRIVATE, count, arg, other, 0);
}
static void *exit_fast(void *arg) { return arg; }
static void *waiter(void *arg)
{
    (void)arg;
    struct timespec timeout = { .tv_sec = 2 };
    long rc = futex(&words[0], WAIT, 1, timed ? &timeout : NULL, NULL);
    intptr_t result = rc == 0 ? 0 : -errno;
    __atomic_store_n(&waiter_done, 1, __ATOMIC_RELEASE);
    return (void *)result;
}
static int fast_joins(void)
{
    for (intptr_t i = 1; i <= 2000; i++) {
        pthread_t thread;
        void *result = NULL;
        if (pthread_create(&thread, NULL, exit_fast, (void *)i) ||
            pthread_join(thread, &result) || result != (void *)i) return 0;
    }
    return 1;
}
static int moved_waiters(int with_timeout)
{
    timed = with_timeout;
    int wake_count = 0;
    for (int i = 0; i < 200; i++) {
        words[0] = words[16] = 1;
        __atomic_store_n(&waiter_done, 0, __ATOMIC_RELEASE);
        pthread_t thread;
        if (pthread_create(&thread, NULL, waiter, NULL)) return 0;
        /* REQUEUE returns one only after the waiter has published its entry.
         * The target is 64 bytes away, in a different AzamiOS hash bucket. */
        long moved;
        do {
            moved = futex(&words[0], REQUEUE, 0, (void *)1, &words[16]);
            if (moved == 0 && __atomic_load_n(&waiter_done, __ATOMIC_ACQUIRE)) {
                void *result;
                pthread_join(thread, &result);
                if (timed && (intptr_t)result == -ETIMEDOUT &&
                    futex(&words[0], WAKE, 1, NULL, NULL) == 0 &&
                    futex(&words[16], WAKE, 1, NULL, NULL) == 0) break;
                printf("  waiter exited before requeue: timed=%d iteration=%d rc=%ld\n",
                       timed, i, (long)(intptr_t)result);
                return 0;
            }
            if (moved == 0) sched_yield();
        } while (moved == 0);
        if (moved == 0) continue; /* Deadline can win before requeue. */
        if (moved != 1) {
            printf("  requeue failed: timed=%d iteration=%d rc=%ld errno=%d\n", timed, i, moved, errno);
            return 0;
        }
        long woke = futex(&words[16], WAKE, 1, NULL, NULL);
        void *result = (void *)1;
        int joined = pthread_join(thread, &result);
        if (joined || !((woke == 1 && !result) ||
                        (timed && woke == 0 && (intptr_t)result == -ETIMEDOUT))) {
            printf("  wake/join failed: timed=%d iteration=%d wake=%ld result=%ld\n",
                   timed, i, woke, (long)(intptr_t)result);
            return 0;
        }
        if (woke == 1) wake_count++;
        /* Exercise the former stale stack entry after the wait has returned. */
        if (futex(&words[16], WAKE, 1, NULL, NULL) != 0) return 0;
    }
    return wake_count > 0;
}
int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    alarm(60);
    puts("== futex wake and requeue regression ==");
    check(fast_joins(), "2000 immediate pthread exits and joins");
    check(moved_waiters(0), "200 requeued untimed waits and queue cleanup");
    check(moved_waiters(1), "200 timed wait wake/expiry races and queue cleanup");
    words[0] = 0;
    check(futex(&words[0], WAIT, 1, NULL, NULL) == -1 && errno == EAGAIN,
          "changed value returns EAGAIN without waiting");
    struct timespec timeout = { .tv_nsec = 20000000 };
    check(futex(&words[0], WAIT, 0, &timeout, NULL) == -1 && errno == ETIMEDOUT,
          "matching value expires with ETIMEDOUT");
    printf("== probe complete: %d passed, %d failed ==\n", passed, failed);
    return failed != 0;
}
