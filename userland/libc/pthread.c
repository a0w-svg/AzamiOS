/* ============================================================================
 * AzamiOS Userspace — POSIX Threads Implementation (pthread.c)
 * File: userland/libc/pthread.c
 * ============================================================================ */

#include "include/pthread.h"
#include "include/sys/syscall.h"
#include "include/stdlib.h"
#include "include/unistd.h"

int pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start_routine)(void *), void *arg)
{
    (void)attr;
    long tid = syscall2(SYS_AZ_THREAD_CREATE, (long)start_routine, (long)arg);
    if (tid < 0) return -1;
    if (thread) *thread = (pthread_t)tid;
    return 0;
}

int pthread_join(pthread_t thread, void **retval)
{
    (void)retval;
    /* Wait for target thread */
    int status = 0;
    return (int)syscall3(SYS_wait4, (int)thread, (long)&status, 0);
}

void pthread_exit(void *retval)
{
    (void)retval;
    sys_exit(0);
}

pthread_t pthread_self(void)
{
    return (pthread_t)sys_getpid();
}

int pthread_equal(pthread_t t1, pthread_t t2)
{
    return t1 == t2;
}

int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr)
{
    (void)attr;
    if (!mutex) return -1;
    mutex->lock = 0;
    mutex->owner = 0;
    mutex->count = 0;
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *mutex)
{
    if (!mutex) return -1;
    mutex->lock = 0;
    return 0;
}

int pthread_mutex_lock(pthread_mutex_t *mutex)
{
    if (!mutex) return -1;
    pthread_t me = pthread_self();
    if (mutex->owner == me) {
        mutex->count++;
        return 0;
    }

    while (__sync_lock_test_and_set(&mutex->lock, 1)) {
        syscall0(SYS_AZ_YIELD);
    }
    mutex->owner = me;
    mutex->count = 1;
    return 0;
}

int pthread_mutex_trylock(pthread_mutex_t *mutex)
{
    if (!mutex) return -1;
    pthread_t me = pthread_self();
    if (mutex->owner == me) {
        mutex->count++;
        return 0;
    }

    if (__sync_lock_test_and_set(&mutex->lock, 1) == 0) {
        mutex->owner = me;
        mutex->count = 1;
        return 0;
    }
    return -1;
}

int pthread_mutex_unlock(pthread_mutex_t *mutex)
{
    if (!mutex) return -1;
    if (mutex->owner != pthread_self()) return -1;

    mutex->count--;
    if (mutex->count == 0) {
        mutex->owner = 0;
        __sync_lock_release(&mutex->lock);
    }
    return 0;
}

int pthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr)
{
    (void)attr;
    if (!cond) return -1;
    cond->seq = 0;
    return 0;
}

int pthread_cond_destroy(pthread_cond_t *cond)
{
    (void)cond;
    return 0;
}

int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex)
{
    if (!cond || !mutex) return -1;
    int seq = cond->seq;
    pthread_mutex_unlock(mutex);

    while (cond->seq == seq) {
        syscall0(SYS_AZ_YIELD);
    }

    pthread_mutex_lock(mutex);
    return 0;
}

int pthread_cond_signal(pthread_cond_t *cond)
{
    if (!cond) return -1;
    __sync_fetch_and_add(&cond->seq, 1);
    return 0;
}

int pthread_cond_broadcast(pthread_cond_t *cond)
{
    if (!cond) return -1;
    __sync_fetch_and_add(&cond->seq, 1);
    return 0;
}
