/* ============================================================================
 * AzamiOS Userspace — POSIX Threads Header (pthread.h)
 * File: userland/libc/include/pthread.h
 * ============================================================================ */
#pragma once

#include "sys/types.h"

typedef unsigned long pthread_t;
typedef unsigned int  pthread_key_t;
typedef volatile int  pthread_once_t;

#define PTHREAD_ONCE_INIT 0

#define PTHREAD_PROCESS_PRIVATE 0
#define PTHREAD_PROCESS_SHARED  1

#define PTHREAD_CREATE_JOINABLE 0
#define PTHREAD_CREATE_DETACHED 1

#define PTHREAD_BARRIER_SERIAL_THREAD 1

typedef struct {
    int is_initialized;
    void *stack_addr;
    size_t stack_size;
    int detach_state;
} pthread_attr_t;

typedef struct {
    volatile int lock;
    pthread_t owner;
    int count;
} pthread_mutex_t;

typedef struct {
    int dummy;
} pthread_mutexattr_t;

typedef struct {
    volatile int seq;
} pthread_cond_t;

typedef struct {
    int dummy;
} pthread_condattr_t;

typedef struct {
    volatile int lock; /* >0: reader count, -1: writer locked, 0: unlocked */
} pthread_rwlock_t;

typedef struct {
    int dummy;
} pthread_rwlockattr_t;

typedef volatile int pthread_spinlock_t;

typedef struct {
    unsigned int count;
    volatile unsigned int in;
    volatile unsigned int cycle;
} pthread_barrier_t;

typedef struct {
    int dummy;
} pthread_barrierattr_t;

#define PTHREAD_MUTEX_INITIALIZER  { 0, 0, 0 }
#define PTHREAD_COND_INITIALIZER   { 0 }
#define PTHREAD_RWLOCK_INITIALIZER { 0 }
#define PTHREAD_SPINLOCK_INITIALIZER 0

/* ── Thread management ───────────────────────────────────────────────────── */
int       pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start_routine)(void *), void *arg);
int       pthread_join(pthread_t thread, void **retval);
int       pthread_detach(pthread_t thread);
void      pthread_exit(void *retval) __attribute__((noreturn));
pthread_t pthread_self(void);
int       pthread_equal(pthread_t t1, pthread_t t2);
int       pthread_once(pthread_once_t *once_control, void (*init_routine)(void));

/* ── Thread-Specific Data (TLS) ──────────────────────────────────────────── */
int   pthread_key_create(pthread_key_t *key, void (*destructor)(void *));
int   pthread_key_delete(pthread_key_t key);
int   pthread_setspecific(pthread_key_t key, const void *value);
void *pthread_getspecific(pthread_key_t key);

/* ── Mutexes ─────────────────────────────────────────────────────────────── */
int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr);
int pthread_mutex_destroy(pthread_mutex_t *mutex);
int pthread_mutex_lock(pthread_mutex_t *mutex);
int pthread_mutex_trylock(pthread_mutex_t *mutex);
int pthread_mutex_unlock(pthread_mutex_t *mutex);

/* ── Condition Variables ─────────────────────────────────────────────────── */
int pthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr);
int pthread_cond_destroy(pthread_cond_t *cond);
int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex);
int pthread_cond_signal(pthread_cond_t *cond);
int pthread_cond_broadcast(pthread_cond_t *cond);

/* ── Read-Write Locks ────────────────────────────────────────────────────── */
int pthread_rwlock_init(pthread_rwlock_t *rwlock, const pthread_rwlockattr_t *attr);
int pthread_rwlock_destroy(pthread_rwlock_t *rwlock);
int pthread_rwlock_rdlock(pthread_rwlock_t *rwlock);
int pthread_rwlock_tryrdlock(pthread_rwlock_t *rwlock);
int pthread_rwlock_wrlock(pthread_rwlock_t *rwlock);
int pthread_rwlock_trywrlock(pthread_rwlock_t *rwlock);
int pthread_rwlock_unlock(pthread_rwlock_t *rwlock);

/* ── Spinlocks ───────────────────────────────────────────────────────────── */
int pthread_spin_init(pthread_spinlock_t *lock, int pshared);
int pthread_spin_destroy(pthread_spinlock_t *lock);
int pthread_spin_lock(pthread_spinlock_t *lock);
int pthread_spin_trylock(pthread_spinlock_t *lock);
int pthread_spin_unlock(pthread_spinlock_t *lock);

/* ── Barriers ────────────────────────────────────────────────────────────── */
int pthread_barrier_init(pthread_barrier_t *barrier, const pthread_barrierattr_t *attr, unsigned int count);
int pthread_barrier_destroy(pthread_barrier_t *barrier);
int pthread_barrier_wait(pthread_barrier_t *barrier);
