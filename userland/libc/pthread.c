/* ============================================================================
 * AzamiOS Userspace — POSIX Threads Implementation (pthread.c)
 * File: userland/libc/pthread.c
 * ============================================================================ */

#include "include/pthread.h"
#include "include/sys/syscall.h"
#include "include/stdlib.h"
#include "include/unistd.h"
#include "include/errno.h"

#define DEFAULT_THREAD_STACK_SIZE (64 * 1024) /* 64 KB */
#define PTHREAD_KEYS_MAX 64

typedef struct {
    void *(*start_routine)(void *);
    void *arg;
} thread_startup_ctx_t;

static void thread_startup_trampoline(void *raw_ctx)
{
    thread_startup_ctx_t *ctx = (thread_startup_ctx_t *)raw_ctx;
    void *(*fn)(void *) = ctx->start_routine;
    void *arg = ctx->arg;
    free(ctx);

    void *ret = fn(arg);
    pthread_exit(ret);
}

int pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start_routine)(void *), void *arg)
{
    size_t stack_size = (attr && attr->stack_size) ? attr->stack_size : DEFAULT_THREAD_STACK_SIZE;
    void *stack = malloc(stack_size);
    if (!stack) return -1;

    /* Top of stack (aligned to 16 bytes for System V AMD64 ABI) */
    uintptr_t stack_top = ((uintptr_t)stack + stack_size - 16) & ~0xFULL;

    thread_startup_ctx_t *ctx = (thread_startup_ctx_t *)malloc(sizeof(thread_startup_ctx_t));
    if (!ctx) {
        free(stack);
        return -1;
    }
    ctx->start_routine = start_routine;
    ctx->arg = arg;

    long tid = syscall3(SYS_AZ_THREAD_CREATE, (long)thread_startup_trampoline, (long)stack_top, (long)ctx);
    if (tid < 0) {
        free(ctx);
        free(stack);
        return -1;
    }

    if (thread) *thread = (pthread_t)tid;
    return 0;
}

int pthread_join(pthread_t thread, void **retval)
{
    (void)retval;
    int status = 0;
    return (int)syscall3(SYS_wait4, (int)thread, (long)&status, 0);
}

int pthread_detach(pthread_t thread)
{
    (void)thread;
    return 0;
}

void pthread_exit(void *retval)
{
    (void)retval;
    syscall1(SYS_AZ_THREAD_EXIT, (long)retval);
    for (;;) {
        syscall0(SYS_AZ_YIELD);
    }
}

pthread_t pthread_self(void)
{
    return (pthread_t)sys_getpid();
}

int pthread_equal(pthread_t t1, pthread_t t2)
{
    return t1 == t2;
}

int pthread_once(pthread_once_t *once_control, void (*init_routine)(void))
{
    if (!once_control || !init_routine) return EINVAL;

    if (*once_control == 2) return 0;

    while (__sync_val_compare_and_swap(once_control, 0, 1) != 0) {
        if (*once_control == 2) return 0;
        syscall0(SYS_AZ_YIELD);
    }

    init_routine();
    *once_control = 2;
    return 0;
}

/* ── Thread-Specific Data (TLS) ──────────────────────────────────────────── */

typedef struct {
    int in_use;
    void (*destructor)(void *);
} tls_key_entry_t;

static tls_key_entry_t g_tls_keys[PTHREAD_KEYS_MAX];
static const void *g_tls_values[PTHREAD_KEYS_MAX];

int pthread_key_create(pthread_key_t *key, void (*destructor)(void *))
{
    if (!key) return EINVAL;
    for (unsigned int i = 0; i < PTHREAD_KEYS_MAX; i++) {
        if (!g_tls_keys[i].in_use) {
            g_tls_keys[i].in_use = 1;
            g_tls_keys[i].destructor = destructor;
            g_tls_values[i] = NULL;
            *key = i;
            return 0;
        }
    }
    return EAGAIN;
}

int pthread_key_delete(pthread_key_t key)
{
    if (key >= PTHREAD_KEYS_MAX || !g_tls_keys[key].in_use) return EINVAL;
    g_tls_keys[key].in_use = 0;
    g_tls_keys[key].destructor = NULL;
    g_tls_values[key] = NULL;
    return 0;
}

int pthread_setspecific(pthread_key_t key, const void *value)
{
    if (key >= PTHREAD_KEYS_MAX || !g_tls_keys[key].in_use) return EINVAL;
    g_tls_values[key] = value;
    return 0;
}

void *pthread_getspecific(pthread_key_t key)
{
    if (key >= PTHREAD_KEYS_MAX || !g_tls_keys[key].in_use) return NULL;
    return (void *)g_tls_values[key];
}

/* ── Mutexes ─────────────────────────────────────────────────────────────── */

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

    int spin = 0;
    while (__sync_lock_test_and_set(&mutex->lock, 1)) {
        if (++spin < 100) {
            __asm__ volatile("pause");
        } else {
            syscall0(SYS_AZ_YIELD);
            spin = 0;
        }
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

/* ── Condition Variables ─────────────────────────────────────────────────── */

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

/* ── Read-Write Locks ────────────────────────────────────────────────────── */

int pthread_rwlock_init(pthread_rwlock_t *rwlock, const pthread_rwlockattr_t *attr)
{
    (void)attr;
    if (!rwlock) return EINVAL;
    rwlock->lock = 0;
    return 0;
}

int pthread_rwlock_destroy(pthread_rwlock_t *rwlock)
{
    if (!rwlock) return EINVAL;
    rwlock->lock = 0;
    return 0;
}

int pthread_rwlock_rdlock(pthread_rwlock_t *rwlock)
{
    if (!rwlock) return EINVAL;
    for (;;) {
        int v = rwlock->lock;
        if (v >= 0) {
            if (__sync_bool_compare_and_swap(&rwlock->lock, v, v + 1)) return 0;
        }
        syscall0(SYS_AZ_YIELD);
    }
}

int pthread_rwlock_tryrdlock(pthread_rwlock_t *rwlock)
{
    if (!rwlock) return EINVAL;
    int v = rwlock->lock;
    if (v >= 0) {
        if (__sync_bool_compare_and_swap(&rwlock->lock, v, v + 1)) return 0;
    }
    return EBUSY;
}

int pthread_rwlock_wrlock(pthread_rwlock_t *rwlock)
{
    if (!rwlock) return EINVAL;
    for (;;) {
        if (__sync_bool_compare_and_swap(&rwlock->lock, 0, -1)) return 0;
        syscall0(SYS_AZ_YIELD);
    }
}

int pthread_rwlock_trywrlock(pthread_rwlock_t *rwlock)
{
    if (!rwlock) return EINVAL;
    if (__sync_bool_compare_and_swap(&rwlock->lock, 0, -1)) return 0;
    return EBUSY;
}

int pthread_rwlock_unlock(pthread_rwlock_t *rwlock)
{
    if (!rwlock) return EINVAL;
    for (;;) {
        int v = rwlock->lock;
        if (v == -1) {
            if (__sync_bool_compare_and_swap(&rwlock->lock, -1, 0)) return 0;
        } else if (v > 0) {
            if (__sync_bool_compare_and_swap(&rwlock->lock, v, v - 1)) return 0;
        } else {
            return EINVAL;
        }
    }
}

/* ── Spinlocks ───────────────────────────────────────────────────────────── */

int pthread_spin_init(pthread_spinlock_t *lock, int pshared)
{
    (void)pshared;
    if (!lock) return EINVAL;
    *lock = 0;
    return 0;
}

int pthread_spin_destroy(pthread_spinlock_t *lock)
{
    if (!lock) return EINVAL;
    *lock = 0;
    return 0;
}

int pthread_spin_lock(pthread_spinlock_t *lock)
{
    if (!lock) return EINVAL;
    while (__sync_lock_test_and_set(lock, 1)) {
        while (*lock) {
            __asm__ volatile("pause");
        }
    }
    return 0;
}

int pthread_spin_trylock(pthread_spinlock_t *lock)
{
    if (!lock) return EINVAL;
    if (__sync_lock_test_and_set(lock, 1) == 0) return 0;
    return EBUSY;
}

int pthread_spin_unlock(pthread_spinlock_t *lock)
{
    if (!lock) return EINVAL;
    __sync_lock_release(lock);
    return 0;
}

/* ── Barriers ────────────────────────────────────────────────────────────── */

int pthread_barrier_init(pthread_barrier_t *barrier, const pthread_barrierattr_t *attr, unsigned int count)
{
    (void)attr;
    if (!barrier || count == 0) return EINVAL;
    barrier->count = count;
    barrier->in = 0;
    barrier->cycle = 0;
    return 0;
}

int pthread_barrier_destroy(pthread_barrier_t *barrier)
{
    if (!barrier) return EINVAL;
    return 0;
}

int pthread_barrier_wait(pthread_barrier_t *barrier)
{
    if (!barrier) return EINVAL;
    unsigned int cycle = barrier->cycle;
    unsigned int in = __sync_add_and_fetch(&barrier->in, 1);

    if (in == barrier->count) {
        barrier->in = 0;
        __sync_fetch_and_add(&barrier->cycle, 1);
        return PTHREAD_BARRIER_SERIAL_THREAD;
    }

    while (barrier->cycle == cycle) {
        syscall0(SYS_AZ_YIELD);
    }
    return 0;
}
