/* ============================================================================
 * AzamiOS Userspace — POSIX Threads Header (pthread.h)
 * File: userland/libc/include/pthread.h
 * ============================================================================ */
#pragma once

#include "sys/types.h"

typedef unsigned long pthread_t;

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

#define PTHREAD_MUTEX_INITIALIZER { 0, 0, 0 }
#define PTHREAD_COND_INITIALIZER  { 0 }

#define PTHREAD_CREATE_JOINABLE 0
#define PTHREAD_CREATE_DETACHED 1

int pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start_routine)(void *), void *arg);
int pthread_join(pthread_t thread, void **retval);
void pthread_exit(void *retval) __attribute__((noreturn));
pthread_t pthread_self(void);
int pthread_equal(pthread_t t1, pthread_t t2);

int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr);
int pthread_mutex_destroy(pthread_mutex_t *mutex);
int pthread_mutex_lock(pthread_mutex_t *mutex);
int pthread_mutex_trylock(pthread_mutex_t *mutex);
int pthread_mutex_unlock(pthread_mutex_t *mutex);

int pthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr);
int pthread_cond_destroy(pthread_cond_t *cond);
int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex);
int pthread_cond_signal(pthread_cond_t *cond);
int pthread_cond_broadcast(pthread_cond_t *cond);
