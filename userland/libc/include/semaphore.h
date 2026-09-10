/* ============================================================================
 * AzamiOS Userspace — POSIX Semaphores (semaphore.h)
 * File: userland/libc/include/semaphore.h
 * ============================================================================ */
#pragma once

#include "sys/types.h"
#include "time.h"
#include "fcntl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    volatile int value;
    int          pshared;
    int          is_named;
    int          fd;
} sem_t;

#define SEM_FAILED ((sem_t *)0)
#define SEM_VALUE_MAX 32767

int    sem_init(sem_t *sem, int pshared, unsigned int value);
int    sem_destroy(sem_t *sem);
sem_t *sem_open(const char *name, int oflag, ...);
int    sem_close(sem_t *sem);
int    sem_unlink(const char *name);
int    sem_wait(sem_t *sem);
int    sem_trywait(sem_t *sem);
int    sem_timedwait(sem_t *sem, const struct timespec *abs_timeout);
int    sem_post(sem_t *sem);
int    sem_getvalue(sem_t *sem, int *sval);

#ifdef __cplusplus
}
#endif
