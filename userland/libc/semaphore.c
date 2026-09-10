/* ============================================================================
 * AzamiOS Userspace — POSIX Semaphores Implementation
 * File: userland/libc/semaphore.c
 * ============================================================================ */

#include "include/semaphore.h"
#include "include/sys/syscall.h"
#include "include/unistd.h"
#include "include/stdlib.h"
#include "include/errno.h"
#include <stdarg.h>

static inline long __sem_ret(long r)
{
    if (r < 0) {
        errno = (int)-r;
        return -1;
    }
    return r;
}

int sem_init(sem_t *sem, int pshared, unsigned int value)
{
    if (!sem || value > SEM_VALUE_MAX) {
        errno = EINVAL;
        return -1;
    }
    sem->value = (int)value;
    sem->pshared = pshared;
    sem->is_named = 0;
    sem->fd = -1;
    return 0;
}

int sem_destroy(sem_t *sem)
{
    if (!sem) {
        errno = EINVAL;
        return -1;
    }
    sem->value = 0;
    return 0;
}

sem_t *sem_open(const char *name, int oflag, ...)
{
    unsigned int mode = 0;
    unsigned int value = 0;

    if (oflag & O_CREAT) {
        va_list ap;
        va_start(ap, oflag);
        mode = va_arg(ap, unsigned int);
        value = va_arg(ap, unsigned int);
        va_end(ap);
    }

    long fd = syscall4(SYS_AZ_SEM_OPEN, (long)name, oflag, mode, value);
    if (fd < 0) {
        errno = (int)-fd;
        return SEM_FAILED;
    }

    sem_t *sem = (sem_t *)malloc(sizeof(sem_t));
    if (!sem) {
        close((int)fd);
        errno = ENOMEM;
        return SEM_FAILED;
    }

    sem->value = 0;
    sem->pshared = 1;
    sem->is_named = 1;
    sem->fd = (int)fd;
    return sem;
}

int sem_close(sem_t *sem)
{
    if (!sem || !sem->is_named) {
        errno = EINVAL;
        return -1;
    }
    long ret = syscall1(SYS_AZ_SEM_CLOSE, sem->fd);
    free(sem);
    return (int)__sem_ret(ret);
}

int sem_unlink(const char *name)
{
    return (int)__sem_ret(syscall1(SYS_AZ_SEM_UNLINK, (long)name));
}

int sem_post(sem_t *sem)
{
    if (!sem) {
        errno = EINVAL;
        return -1;
    }
    if (sem->is_named) {
        return (int)__sem_ret(syscall1(SYS_AZ_SEM_POST, sem->fd));
    }

    __sync_fetch_and_add(&sem->value, 1);
    return 0;
}

int sem_wait(sem_t *sem)
{
    if (!sem) {
        errno = EINVAL;
        return -1;
    }
    if (sem->is_named) {
        return (int)__sem_ret(syscall1(SYS_AZ_SEM_WAIT, sem->fd));
    }

    while (1) {
        while (sem->value <= 0) {
            __asm__ volatile("pause");
        }
        if (__sync_bool_compare_and_swap(&sem->value, sem->value, sem->value - 1)) {
            return 0;
        }
    }
}

int sem_trywait(sem_t *sem)
{
    if (!sem) {
        errno = EINVAL;
        return -1;
    }
    if (sem->is_named) {
        return (int)__sem_ret(syscall1(SYS_AZ_SEM_TRYWAIT, sem->fd));
    }

    int v = sem->value;
    if (v <= 0) {
        errno = EAGAIN;
        return -1;
    }
    if (__sync_bool_compare_and_swap(&sem->value, v, v - 1)) {
        return 0;
    }
    errno = EAGAIN;
    return -1;
}

int sem_timedwait(sem_t *sem, const struct timespec *abs_timeout)
{
    if (!sem) {
        errno = EINVAL;
        return -1;
    }
    if (sem->is_named) {
        return (int)__sem_ret(syscall2(SYS_AZ_SEM_TIMEDWAIT, sem->fd, (long)abs_timeout));
    }

    while (sem_trywait(sem) != 0) {
        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        if (abs_timeout && (now.tv_sec > abs_timeout->tv_sec ||
            (now.tv_sec == abs_timeout->tv_sec && now.tv_nsec >= abs_timeout->tv_nsec))) {
            errno = ETIMEDOUT;
            return -1;
        }
        __asm__ volatile("pause");
    }
    return 0;
}

int sem_getvalue(sem_t *sem, int *sval)
{
    if (!sem || !sval) {
        errno = EINVAL;
        return -1;
    }
    if (sem->is_named) {
        return (int)__sem_ret(syscall2(SYS_AZ_SEM_GETVALUE, sem->fd, (long)sval));
    }

    *sval = sem->value;
    return 0;
}
