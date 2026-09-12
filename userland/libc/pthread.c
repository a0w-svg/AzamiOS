/* ============================================================================
 * AzamiOS Userspace — POSIX Threads Implementation (pthread.c)
 * File: userland/libc/pthread.c
 * ============================================================================ */

#include "include/pthread.h"
#include "include/sys/syscall.h"
#include "include/stdlib.h"
#include "include/unistd.h"
#include "include/errno.h"

#define FUTEX_WAIT_PRIVATE 128
#define FUTEX_WAKE_PRIVATE 129

#define DEFAULT_THREAD_STACK_SIZE (64 * 1024) /* 64 KB */
#define PTHREAD_KEYS_MAX 64

/* ── Join bookkeeping ────────────────────────────────────────────────────
 *
 * pthread_join() uses futex(FUTEX_WAIT_PRIVATE) to wait for thread completion
 * with zero CPU spin/yield overhead. When the thread finishes, the trampoline
 * stores done = 1 and calls futex(FUTEX_WAKE_PRIVATE). */
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
    int exp = 0;
    if (__atomic_compare_exchange_n(&g_join_lock, &exp, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        return;
    for (int i = 0; i < 100; i++) {
        __asm__ volatile("pause");
        exp = 0;
        if (__atomic_compare_exchange_n(&g_join_lock, &exp, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            return;
    }
    while (1) {
        if (exp == 2 || __atomic_exchange_n(&g_join_lock, 2, __ATOMIC_ACQ_REL) != 0) {
            syscall4(SYS_futex, (long)&g_join_lock, FUTEX_WAIT_PRIVATE, 2, 0);
        }
        exp = 0;
        if (__atomic_compare_exchange_n(&g_join_lock, &exp, 2, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            return;
    }
}

static void join_unlock(void)
{
    if (__atomic_exchange_n(&g_join_lock, 0, __ATOMIC_RELEASE) == 2) {
        syscall4(SYS_futex, (long)&g_join_lock, FUTEX_WAKE_PRIVATE, 1, 0);
    }
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
        __atomic_store_n(&slot->done, 1, __ATOMIC_RELEASE);
        syscall4(SYS_futex, (long)&slot->done, FUTEX_WAKE_PRIVATE, 1, 0);
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

    while (!__atomic_load_n(&slot->done, __ATOMIC_ACQUIRE)) {
        syscall4(SYS_futex, (long)&slot->done, FUTEX_WAIT_PRIVATE, 0, 0);
    }
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

    /* Fast path: 0 -> 1 */
    int exp = 0;
    if (__atomic_compare_exchange_n(&mutex->lock, &exp, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        mutex->owner = me;
        mutex->count = 1;
        return 0;
    }

    /* Adaptive spin */
    for (int i = 0; i < 100; i++) {
        __asm__ volatile("pause");
        exp = 0;
        if (__atomic_compare_exchange_n(&mutex->lock, &exp, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            mutex->owner = me;
            mutex->count = 1;
            return 0;
        }
    }

    /* Contended slow path: mark 2 (has waiters) and sleep in futex */
    while (1) {
        if (exp == 2 || __atomic_exchange_n(&mutex->lock, 2, __ATOMIC_ACQ_REL) != 0) {
            syscall4(SYS_futex, (long)&mutex->lock, FUTEX_WAIT_PRIVATE, 2, 0);
        }
        exp = 0;
        if (__atomic_compare_exchange_n(&mutex->lock, &exp, 2, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            mutex->owner = me;
            mutex->count = 1;
            return 0;
        }
    }
}

int pthread_mutex_trylock(pthread_mutex_t *mutex)
{
    if (!mutex) return -1;
    pthread_t me = pthread_self();
    if (mutex->owner == me) {
        mutex->count++;
        return 0;
    }

    int exp = 0;
    if (__atomic_compare_exchange_n(&mutex->lock, &exp, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
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
        /* If previous value was 2, there were waiters to wake */
        if (__atomic_exchange_n(&mutex->lock, 0, __ATOMIC_RELEASE) == 2) {
            syscall4(SYS_futex, (long)&mutex->lock, FUTEX_WAKE_PRIVATE, 1, 0);
        }
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
    int seq = __atomic_load_n(&cond->seq, __ATOMIC_ACQUIRE);
    pthread_mutex_unlock(mutex);

    while (__atomic_load_n(&cond->seq, __ATOMIC_ACQUIRE) == seq) {
        syscall4(SYS_futex, (long)&cond->seq, FUTEX_WAIT_PRIVATE, seq, 0);
    }

    pthread_mutex_lock(mutex);
    return 0;
}

int pthread_cond_signal(pthread_cond_t *cond)
{
    if (!cond) return -1;
    __atomic_add_fetch(&cond->seq, 1, __ATOMIC_RELEASE);
    syscall4(SYS_futex, (long)&cond->seq, FUTEX_WAKE_PRIVATE, 1, 0);
    return 0;
}

int pthread_cond_broadcast(pthread_cond_t *cond)
{
    if (!cond) return -1;
    __atomic_add_fetch(&cond->seq, 1, __ATOMIC_RELEASE);
    syscall4(SYS_futex, (long)&cond->seq, FUTEX_WAKE_PRIVATE, 0x7FFFFFFF, 0);
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
            if (__atomic_compare_exchange_n(&rwlock->lock, &v, v + 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return 0;
        }
        for (int i = 0; i < 64; i++) {
            __asm__ volatile("pause");
            v = rwlock->lock;
            if (v >= 0 && __atomic_compare_exchange_n(&rwlock->lock, &v, v + 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return 0;
        }
        syscall4(SYS_futex, (long)&rwlock->lock, FUTEX_WAIT_PRIVATE, v, 0);
    }
}

int pthread_rwlock_tryrdlock(pthread_rwlock_t *rwlock)
{
    if (!rwlock) return EINVAL;
    int v = rwlock->lock;
    if (v >= 0) {
        if (__atomic_compare_exchange_n(&rwlock->lock, &v, v + 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return 0;
    }
    return EBUSY;
}

int pthread_rwlock_wrlock(pthread_rwlock_t *rwlock)
{
    if (!rwlock) return EINVAL;
    for (;;) {
        int v = 0;
        if (__atomic_compare_exchange_n(&rwlock->lock, &v, -1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return 0;
        for (int i = 0; i < 64; i++) {
            __asm__ volatile("pause");
            v = 0;
            if (__atomic_compare_exchange_n(&rwlock->lock, &v, -1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return 0;
        }
        syscall4(SYS_futex, (long)&rwlock->lock, FUTEX_WAIT_PRIVATE, rwlock->lock, 0);
    }
}

int pthread_rwlock_trywrlock(pthread_rwlock_t *rwlock)
{
    if (!rwlock) return EINVAL;
    int v = 0;
    if (__atomic_compare_exchange_n(&rwlock->lock, &v, -1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return 0;
    return EBUSY;
}

int pthread_rwlock_unlock(pthread_rwlock_t *rwlock)
{
    if (!rwlock) return EINVAL;
    for (;;) {
        int v = rwlock->lock;
        if (v == -1) {
            if (__atomic_compare_exchange_n(&rwlock->lock, &v, 0, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
                syscall4(SYS_futex, (long)&rwlock->lock, FUTEX_WAKE_PRIVATE, 0x7FFFFFFF, 0);
                return 0;
            }
        } else if (v > 0) {
            if (__atomic_compare_exchange_n(&rwlock->lock, &v, v - 1, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
                if (v == 1) {
                    syscall4(SYS_futex, (long)&rwlock->lock, FUTEX_WAKE_PRIVATE, 1, 0);
                }
                return 0;
            }
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
    unsigned int cycle = __atomic_load_n(&barrier->cycle, __ATOMIC_ACQUIRE);
    unsigned int in = __atomic_add_fetch(&barrier->in, 1, __ATOMIC_SEQ_CST);

    if (in == barrier->count) {
        barrier->in = 0;
        __atomic_add_fetch(&barrier->cycle, 1, __ATOMIC_RELEASE);
        syscall4(SYS_futex, (long)&barrier->cycle, FUTEX_WAKE_PRIVATE, barrier->count, 0);
        return PTHREAD_BARRIER_SERIAL_THREAD;
    }

    while (__atomic_load_n(&barrier->cycle, __ATOMIC_ACQUIRE) == cycle) {
        syscall4(SYS_futex, (long)&barrier->cycle, FUTEX_WAIT_PRIVATE, cycle, 0);
    }
    return 0;
}
