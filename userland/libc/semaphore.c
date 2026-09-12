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

#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_WAIT_PRIVATE 128
#define FUTEX_WAKE_PRIVATE 129

int sem_post(sem_t *sem)
{
    if (!sem) {
        errno = EINVAL;
        return -1;
    }
    if (sem->is_named) {
        return (int)__sem_ret(syscall1(SYS_AZ_SEM_POST, sem->fd));
    }

    __atomic_add_fetch(&sem->value, 1, __ATOMIC_RELEASE);
    int op = sem->pshared ? FUTEX_WAKE : FUTEX_WAKE_PRIVATE;
    syscall4(SYS_futex, (long)&sem->value, op, 1, 0);
    return 0;
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

    int v = __atomic_load_n(&sem->value, __ATOMIC_ACQUIRE);
    while (v > 0) {
        if (__atomic_compare_exchange_n(&sem->value, &v, v - 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            return 0;
        }
    }
    errno = EAGAIN;
    return -1;
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

    int op = sem->pshared ? FUTEX_WAIT : FUTEX_WAIT_PRIVATE;
    while (1) {
        int v = __atomic_load_n(&sem->value, __ATOMIC_ACQUIRE);
        while (v > 0) {
            if (__atomic_compare_exchange_n(&sem->value, &v, v - 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
                return 0;
            }
        }
        /* Adaptive pause before going to sleep */
        for (int i = 0; i < 40; i++) {
            __asm__ volatile("pause");
            v = __atomic_load_n(&sem->value, __ATOMIC_ACQUIRE);
            if (v > 0 && __atomic_compare_exchange_n(&sem->value, &v, v - 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
                return 0;
            }
        }
        /* Sleep in futex until next sem_post */
        syscall4(SYS_futex, (long)&sem->value, op, 0, 0);
    }
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

    int op = sem->pshared ? FUTEX_WAIT : FUTEX_WAIT_PRIVATE;
    while (sem_trywait(sem) != 0) {
        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        if (abs_timeout) {
            if (now.tv_sec > abs_timeout->tv_sec ||
                (now.tv_sec == abs_timeout->tv_sec && now.tv_nsec >= abs_timeout->tv_nsec)) {
                errno = ETIMEDOUT;
                return -1;
            }
            struct timespec rel;
            rel.tv_sec = abs_timeout->tv_sec - now.tv_sec;
            rel.tv_nsec = abs_timeout->tv_nsec - now.tv_nsec;
            if (rel.tv_nsec < 0) {
                rel.tv_sec -= 1;
                rel.tv_nsec += 1000000000L;
            }
            syscall4(SYS_futex, (long)&sem->value, op, 0, (long)&rel);
        } else {
            syscall4(SYS_futex, (long)&sem->value, op, 0, 0);
        }
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
