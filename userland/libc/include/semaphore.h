/* ============================================================================
 * AzamiOS Userspace — POSIX Semaphores (semaphore.h)
 * File: userland/libc/include/semaphore.h
 * ============================================================================ */
#pragma once

#include "sys/types.h"
#include "time.h"

typedef struct {
    volatile int value;
    int pshared;
} sem_t;

#define SEM_FAILED ((sem_t *)0)

static inline int sem_init(sem_t *sem, int pshared, unsigned int value)
{
    if (!sem) return -1;
    sem->value = (int)value;
    sem->pshared = pshared;
    return 0;
}

static inline int sem_destroy(sem_t *sem)
{
    if (!sem) return -1;
    sem->value = 0;
    return 0;
}

static inline int sem_post(sem_t *sem)
{
    if (!sem) return -1;
    __sync_fetch_and_add(&sem->value, 1);
    return 0;
}

static inline int sem_trywait(sem_t *sem)
{
    if (!sem) return -1;
    while (1) {
        int v = sem->value;
        if (v <= 0) return -1; /* EAGAIN */
        if (__sync_bool_compare_and_swap(&sem->value, v, v - 1)) return 0;
    }
}

static inline int sem_wait(sem_t *sem)
{
    if (!sem) return -1;
    while (1) {
        while (sem->value <= 0) {
            __asm__ volatile("pause");
        }
        if (__sync_bool_compare_and_swap(&sem->value, sem->value, sem->value - 1)) {
            return 0;
        }
    }
}

static inline int sem_getvalue(sem_t *sem, int *sval)
{
    if (!sem || !sval) return -1;
    *sval = sem->value;
    return 0;
}
