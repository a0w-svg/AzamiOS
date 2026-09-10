/* ============================================================================
 * AzamiOS Userspace — POSIX Named Semaphore Test Suite (sem_test.elf)
 * File: userland/apps/sem_test/main.c
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/string.h"
#include "../../libc/include/semaphore.h"
#include "../../libc/include/fcntl.h"
#include "../../libc/include/unistd.h"
#include "../../libc/include/errno.h"
#include "../../libc/include/sys/wait.h"

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("[sem_test] Starting POSIX named semaphore test suite...\n");

    const char *sem_name = "/az_test_sem";
    sem_unlink(sem_name); /* Clean up if left over from prior runs */

    /* 1. Create semaphore with initial value 1 */
    sem_t *sem = sem_open(sem_name, O_CREAT | O_EXCL, 0644, 1);
    if (sem == SEM_FAILED) {
        perror("sem_open(O_CREAT)");
        return 1;
    }
    printf("[sem_test] sem_open('%s', O_CREAT) -> OK (fd=%d)\n", sem_name, sem->fd);

    /* 2. Check initial value */
    int val = -1;
    if (sem_getvalue(sem, &val) != 0 || val != 1) {
        printf("[sem_test] FAILED: initial value expected 1, got %d\n", val);
        sem_close(sem);
        sem_unlink(sem_name);
        return 1;
    }
    printf("[sem_test] sem_getvalue -> %d (expected 1) -> OK\n", val);

    /* 3. Wait (decrement) */
    if (sem_wait(sem) != 0) {
        perror("sem_wait");
        sem_close(sem);
        sem_unlink(sem_name);
        return 1;
    }
    printf("[sem_test] sem_wait -> OK\n");

    /* 4. Value should now be 0 */
    sem_getvalue(sem, &val);
    if (val != 0) {
        printf("[sem_test] FAILED: value after wait expected 0, got %d\n", val);
        sem_close(sem);
        sem_unlink(sem_name);
        return 1;
    }
    printf("[sem_test] sem_getvalue -> %d (expected 0) -> OK\n", val);

    /* 5. Trywait should fail with EAGAIN */
    if (sem_trywait(sem) == 0) {
        printf("[sem_test] FAILED: sem_trywait succeeded when value was 0!\n");
        sem_close(sem);
        sem_unlink(sem_name);
        return 1;
    }
    if (errno != EAGAIN) {
        printf("[sem_test] FAILED: sem_trywait failed with errno=%d (expected EAGAIN=%d)\n",
               errno, EAGAIN);
        sem_close(sem);
        sem_unlink(sem_name);
        return 1;
    }
    printf("[sem_test] sem_trywait returned EAGAIN as expected -> OK\n");

    /* 6. Post (increment) */
    if (sem_post(sem) != 0) {
        perror("sem_post");
        sem_close(sem);
        sem_unlink(sem_name);
        return 1;
    }
    printf("[sem_test] sem_post -> OK\n");

    sem_getvalue(sem, &val);
    if (val != 1) {
        printf("[sem_test] FAILED: value after post expected 1, got %d\n", val);
        sem_close(sem);
        sem_unlink(sem_name);
        return 1;
    }
    printf("[sem_test] sem_getvalue -> %d (expected 1) -> OK\n", val);

    /* 7. Inter-process synchronization via fork */
    printf("[sem_test] Testing inter-process synchronization with fork...\n");
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        sem_close(sem);
        sem_unlink(sem_name);
        return 1;
    }

    if (pid == 0) {
        printf("  [child] started, opening semaphore '%s'...\n", sem_name);
        sem_t *csem = sem_open(sem_name, 0);
        if (csem == SEM_FAILED) {
            printf("  [child] sem_open failed: errno=%d\n", errno);
            exit(1);
        }
        printf("  [child] posting semaphore...\n");
        if (sem_post(csem) != 0) {
            printf("  [child] sem_post failed: errno=%d\n", errno);
            exit(1);
        }
        printf("  [child] posted semaphore, closing...\n");
        sem_close(csem);
        exit(0);
    } else {
        /* Parent process: decrement twice to wait for child's post */
        /* Currently value is 1. Wait once -> 0 */
        sem_wait(sem);
        printf("  [parent] consumed initial count, now waiting for child post...\n");
        /* Wait again -> blocks until child posts */
        if (sem_wait(sem) != 0) {
            perror("parent sem_wait");
            wait(NULL);
            sem_close(sem);
            sem_unlink(sem_name);
            return 1;
        }
        printf("  [parent] woke up from child post -> OK!\n");
        wait(NULL);
    }

    /* 8. Close and unlink */
    if (sem_close(sem) != 0) {
        perror("sem_close");
        return 1;
    }
    printf("[sem_test] sem_close -> OK\n");

    if (sem_unlink(sem_name) != 0) {
        perror("sem_unlink");
        return 1;
    }
    printf("[sem_test] sem_unlink -> OK\n");

    printf("\n=== All POSIX Named Semaphore Tests PASSED! ===\n");
    return 0;
}
