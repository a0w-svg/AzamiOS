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

/* ── Join bookkeeping ────────────────────────────────────────────────────
 *
 * pthread_join() used to be `wait4(tid, ...)` — but wait4(2) waits on child
 * *processes*, and a pthread_create()'d thread is not one (SYS_AZ_THREAD_CREATE
 * spawns a thread inside the *same* process, sharing its pid). That call
 * returned -ECHILD immediately without blocking at all, so pthread_join()
 * never actually joined anything — confirmed by a regression test
 * (userland/examples/thread_tls_test.c) racing main() reading a worker
 * thread's results against pthread_join() returning instantly.
 *
 * Fixed with a small fixed-size table of join slots: pthread_create()
 * reserves one before the new thread starts running and hands the new
 * thread a pointer to it directly (no need to search by tid later), the
 * new thread's own trampoline marks it done with its return value once
 * start_routine returns, and pthread_join() spins/yields (same pattern as
 * every other lock in this file) until it sees `done`. */
#define PTHREAD_JOIN_MAX 256

typedef struct {
    int in_use;
    pthread_t tid;
    volatile int done;
    void *retval;
} join_slot_t;

static join_slot_t g_join_slots[PTHREAD_JOIN_MAX];
static int g_join_lock = 0;

static void join_lock(void)
{
    int spin = 0;
    while (__sync_lock_test_and_set(&g_join_lock, 1)) {
        if (++spin < 100) { __asm__ volatile("pause"); }
        else { syscall0(SYS_AZ_YIELD); spin = 0; }
    }
}

static void join_unlock(void)
{
    __sync_lock_release(&g_join_lock);
}

/* Reserved for the child before it starts running, so the child never has
 * to search the table (and can't race pthread_join() adding the tid). */
static join_slot_t *join_slot_alloc(void)
{
    join_lock();
    join_slot_t *slot = NULL;
    for (int i = 0; i < PTHREAD_JOIN_MAX; i++) {
        if (!g_join_slots[i].in_use) {
            g_join_slots[i].in_use = 1;
            g_join_slots[i].tid = 0;
            g_join_slots[i].done = 0;
            g_join_slots[i].retval = NULL;
            slot = &g_join_slots[i];
            break;
        }
    }
    join_unlock();
    return slot; /* NULL if all PTHREAD_JOIN_MAX slots are in use */
}

static join_slot_t *join_slot_find(pthread_t tid)
{
    join_slot_t *found = NULL;
    join_lock();
    for (int i = 0; i < PTHREAD_JOIN_MAX; i++) {
        if (g_join_slots[i].in_use && g_join_slots[i].tid == tid) {
            found = &g_join_slots[i];
            break;
        }
    }
    join_unlock();
    return found;
}

typedef struct {
    void *(*start_routine)(void *);
    void *arg;
    join_slot_t *slot; /* NULL if pthread_create() couldn't reserve one —
                         * the thread still runs, it just can't be joined. */
} thread_startup_ctx_t;

/* Sets up this thread's own TLS block (%fs) — see userland/libc/tls.c —
 * before anything the thread does, including touching errno, can rely on
 * per-thread storage. Not declared in a shared header: it is an
 * implementation seam between tls.c and pthread.c only. */
extern void __init_thread_tls(void);

static void thread_startup_trampoline(void *raw_ctx)
{
    __init_thread_tls();

    thread_startup_ctx_t *ctx = (thread_startup_ctx_t *)raw_ctx;
    void *(*fn)(void *) = ctx->start_routine;
    void *arg = ctx->arg;
    join_slot_t *slot = ctx->slot;
    free(ctx);

    void *ret = fn(arg);

    if (slot) {
        slot->retval = ret;
        __sync_synchronize();
        slot->done = 1;
    }
    pthread_exit(ret);
}

int pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start_routine)(void *), void *arg)
{
    size_t stack_size = (attr && attr->stack_size) ? attr->stack_size : DEFAULT_THREAD_STACK_SIZE;
    void *stack = malloc(stack_size);
    if (!stack) return -1;

    /* Top of stack (aligned to 16 bytes for System V AMD64 ABI with 8-byte entry offset) */
    uintptr_t aligned_top = ((uintptr_t)stack + stack_size) & ~0xFULL;
    uintptr_t stack_top = aligned_top - 8;
    *(uint64_t *)stack_top = 0; /* Zero return address to terminate unwinding */

    thread_startup_ctx_t *ctx = (thread_startup_ctx_t *)malloc(sizeof(thread_startup_ctx_t));
    if (!ctx) {
        free(stack);
        return -1;
    }
    ctx->start_routine = start_routine;
    ctx->arg = arg;
    /* All PTHREAD_JOIN_MAX slots in use just means this thread can't be
     * pthread_join()'d — not a reason to fail thread creation outright. */
    ctx->slot = join_slot_alloc();

    long tid = syscall3(SYS_AZ_THREAD_CREATE, (long)thread_startup_trampoline, (long)stack_top, (long)ctx);
    if (tid < 0) {
        if (ctx->slot) { join_lock(); ctx->slot->in_use = 0; join_unlock(); }
        free(ctx);
        free(stack);
        return -1;
    }

    if (ctx->slot) ctx->slot->tid = (pthread_t)tid;
    if (thread) *thread = (pthread_t)tid;
    return 0;
}

int pthread_join(pthread_t thread, void **retval)
{
    join_slot_t *slot = join_slot_find(thread);
    if (!slot) {
        /* Either an invalid tid, already joined, or created when the join
         * table was full — matches ESRCH's real meaning closely enough. */
        return ESRCH;
    }

    int spin = 0;
    while (!slot->done) {
        if (++spin < 100) { __asm__ volatile("pause"); }
        else { syscall0(SYS_AZ_YIELD); spin = 0; }
    }
    __sync_synchronize();
    if (retval) *retval = slot->retval;

    join_lock();
    slot->in_use = 0; /* joining twice is undefined behavior in POSIX too */
    join_unlock();
    return 0;
}

int pthread_detach(pthread_t thread)
{
    (void)thread;
    /* Not tracked as a distinct state: a detached thread's join slot is
     * simply never freed by anyone (PTHREAD_JOIN_MAX total leaked slots in
     * the worst case for an app that detaches every thread it creates) —
     * matches this file's existing, separately-noted "nothing frees a
     * finished thread's stack either" limitation rather than introducing a
     * new one. Freeing it here instead, while the thread may still be
     * running, would be actively wrong: thread_startup_trampoline() writes
     * through the slot pointer it was handed at creation without
     * re-checking in_use, so a slot freed (and possibly reallocated to an
     * unrelated new thread) while the original thread is still running
     * would let that write land on the wrong thread's join state. */
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

/* Cached per-thread: gettid() is a syscall, and pthread_self() is called
 * from every mutex/rwlock operation, so every thread pays for it once.
 *
 * This used to return sys_getpid(), which is the same value for every
 * thread in a process (threads share a pid) — every mutex's
 * `owner == pthread_self()` check therefore treated any thread as already
 * holding the lock, letting two different threads both proceed through a
 * critical section a recursive mutex should have serialized. gettid()
 * (kernel/syscall/syscall.c's sys_gettid_impl) returns the calling
 * thread_t's own tid, which is what SYS_AZ_THREAD_CREATE hands back to
 * pthread_create()'s caller as *thread — the two now agree. */
static __thread pthread_t g_self_tid_cache = 0;

pthread_t pthread_self(void)
{
    if (g_self_tid_cache == 0)
        g_self_tid_cache = (pthread_t)syscall0(SYS_gettid);
    return g_self_tid_cache;
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

/* The key table (which slots are allocated, and their destructors) is
 * process-wide, matching POSIX: a key created by one thread is valid on
 * every thread. Only the values are per-thread. */
static tls_key_entry_t g_tls_keys[PTHREAD_KEYS_MAX];
static __thread const void *g_tls_values[PTHREAD_KEYS_MAX];

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
