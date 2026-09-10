/* ============================================================================
 * AzamiOS — POSIX Named Semaphore Implementation
 * File: kernel/ipc/posix_sem.c
 * ============================================================================ */

#include "posix_sem.h"
#include "../mm/kmalloc.h"
#include "../sched/sched.h"
#include "../../arch/x86_64/cpu/spinlock.h"
#include "../../include/azami/defs.h"
#include "../lib/string.h"
#include "../../drivers/misc/hpet.h"

/* ── Global semaphore name table ─────────────────────────────────────────── */
static posix_sem_t *g_sem_table[POSIX_SEM_TABLE_MAX];
static spinlock_t   g_sem_table_lock = SPINLOCK_INIT;

void posix_sem_init(void)
{
    for (int i = 0; i < POSIX_SEM_TABLE_MAX; i++) g_sem_table[i] = NULL;
}

/* ── Helpers ──────────────────────────────────────────────────────────────── */

static posix_sem_t *sem_table_find_locked(const char *name)
{
    for (int i = 0; i < POSIX_SEM_TABLE_MAX; i++) {
        if (g_sem_table[i] && !g_sem_table[i]->unlinked &&
            strncmp(g_sem_table[i]->name, name, POSIX_SEM_NAME_MAX - 1) == 0)
            return g_sem_table[i];
    }
    return NULL;
}

static int sem_table_insert_locked(posix_sem_t *s)
{
    for (int i = 0; i < POSIX_SEM_TABLE_MAX; i++) {
        if (!g_sem_table[i]) { g_sem_table[i] = s; return 0; }
    }
    return -ENFILE;
}

static void sem_table_remove_locked(posix_sem_t *s)
{
    for (int i = 0; i < POSIX_SEM_TABLE_MAX; i++) {
        if (g_sem_table[i] == s) { g_sem_table[i] = NULL; return; }
    }
}

/* ── Public API ───────────────────────────────────────────────────────────── */

posix_sem_t *posix_sem_open_kern(const char *name, int oflag,
                                 unsigned int mode, unsigned int value)
{
    (void)mode;
    if (!name || name[0] != '/') return NULL;
    if (value > (unsigned int)SEM_VALUE_MAX) return NULL;

    irqflags_t fl = spinlock_lock_irqsave(&g_sem_table_lock);

    posix_sem_t *s = sem_table_find_locked(name);

    if (s) {
        /* Name exists */
        if ((oflag & SEM_O_CREAT) && (oflag & SEM_O_EXCL)) {
            spinlock_unlock_irqrestore(&g_sem_table_lock, fl);
            return NULL; /* EEXIST */
        }
        s->refcount++;
        spinlock_unlock_irqrestore(&g_sem_table_lock, fl);
        return s;
    }

    /* Does not exist */
    if (!(oflag & SEM_O_CREAT)) {
        spinlock_unlock_irqrestore(&g_sem_table_lock, fl);
        return NULL; /* ENOENT */
    }

    posix_sem_t *ns = (posix_sem_t *)kzalloc(sizeof(posix_sem_t));
    if (!ns) {
        spinlock_unlock_irqrestore(&g_sem_table_lock, fl);
        return NULL;
    }
    strncpy(ns->name, name, POSIX_SEM_NAME_MAX - 1);
    ns->name[POSIX_SEM_NAME_MAX - 1] = '\0';
    ns->refcount  = 1;
    ns->unlinked  = false;
    ns->value     = (int)value;
    ns->wait_head = NULL;
    ns->wait_tail = NULL;

    if (sem_table_insert_locked(ns) < 0) {
        spinlock_unlock_irqrestore(&g_sem_table_lock, fl);
        kfree(ns);
        return NULL;
    }

    spinlock_unlock_irqrestore(&g_sem_table_lock, fl);
    return ns;
}

void posix_sem_put(posix_sem_t *sem)
{
    if (!sem) return;

    irqflags_t fl = spinlock_lock_irqsave(&g_sem_table_lock);
    if (sem->refcount > 0)
        sem->refcount--;
    bool should_free = (sem->refcount == 0 && sem->unlinked);
    spinlock_unlock_irqrestore(&g_sem_table_lock, fl);

    if (should_free) kfree(sem);
}

int posix_sem_unlink(const char *name)
{
    if (!name || name[0] != '/') return -(int)EINVAL;

    irqflags_t fl = spinlock_lock_irqsave(&g_sem_table_lock);
    posix_sem_t *s = sem_table_find_locked(name);
    if (!s) {
        spinlock_unlock_irqrestore(&g_sem_table_lock, fl);
        return -(int)ENOENT;
    }
    s->unlinked = true;
    sem_table_remove_locked(s);
    bool should_free = (s->refcount == 0);
    spinlock_unlock_irqrestore(&g_sem_table_lock, fl);

    if (should_free) kfree(s);
    return 0;
}

int posix_sem_post(posix_sem_t *sem)
{
    if (!sem) return -(int)EINVAL;

    irqflags_t fl = spinlock_lock_irqsave(&sem->lock);

    if (sem->value >= SEM_VALUE_MAX) {
        spinlock_unlock_irqrestore(&sem->lock, fl);
        return -(int)EOVERFLOW;
    }

    sem->value++;

    /* Wake the head waiter (FIFO). */
    thread_t *t = sem->wait_head;
    if (t) {
        sem->wait_head = t->sem_next;
        if (!sem->wait_head) sem->wait_tail = NULL;
        t->sem_next = NULL;
        spinlock_unlock_irqrestore(&sem->lock, fl);
        sched_unblock(t);
    } else {
        spinlock_unlock_irqrestore(&sem->lock, fl);
    }
    return 0;
}

int posix_sem_trywait(posix_sem_t *sem)
{
    if (!sem) return -(int)EINVAL;
    irqflags_t fl = spinlock_lock_irqsave(&sem->lock);
    if (sem->value > 0) {
        sem->value--;
        spinlock_unlock_irqrestore(&sem->lock, fl);
        return 0;
    }
    spinlock_unlock_irqrestore(&sem->lock, fl);
    return -(int)EAGAIN;
}

int posix_sem_wait(posix_sem_t *sem)
{
    if (!sem) return -(int)EINVAL;

    for (;;) {
        irqflags_t fl = spinlock_lock_irqsave(&sem->lock);
        if (sem->value > 0) {
            sem->value--;
            spinlock_unlock_irqrestore(&sem->lock, fl);
            return 0;
        }

        /* Enqueue the current thread. */
        thread_t *me = sched_current_thread();
        me->sem_next = NULL;
        if (sem->wait_tail) {
            sem->wait_tail->sem_next = me;
            sem->wait_tail = me;
        } else {
            sem->wait_head = sem->wait_tail = me;
        }
        spinlock_unlock_irqrestore(&sem->lock, fl);

        /* Block; the unblocking party has already incremented the value. */
        sched_block(THREAD_BLOCKED);

        /* Retry — we were woken because value was incremented. */
    }
}

int posix_sem_timedwait(posix_sem_t *sem, u64 abs_timeout_ns)
{
    if (!sem) return -(int)EINVAL;

    for (;;) {
        irqflags_t fl = spinlock_lock_irqsave(&sem->lock);
        if (sem->value > 0) {
            sem->value--;
            spinlock_unlock_irqrestore(&sem->lock, fl);
            return 0;
        }

        /* Check timeout before blocking. */
        u64 now_ns = hpet_available() ? hpet_now_ns()
                                       : sched_get_ticks() * 10000000ULL;
        if (now_ns >= abs_timeout_ns) {
            spinlock_unlock_irqrestore(&sem->lock, fl);
            return -(int)ETIMEDOUT;
        }

        thread_t *me = sched_current_thread();
        me->sem_next = NULL;
        if (sem->wait_tail) {
            sem->wait_tail->sem_next = me;
            sem->wait_tail = me;
        } else {
            sem->wait_head = sem->wait_tail = me;
        }
        spinlock_unlock_irqrestore(&sem->lock, fl);

        /* Sleep until deadline or wakeup. */
        u64 now2_ns = hpet_available() ? hpet_now_ns()
                                       : sched_get_ticks() * 10000000ULL;
        u64 remaining_ns = abs_timeout_ns > now2_ns ? abs_timeout_ns - now2_ns : 0;
        u64 ticks = (remaining_ns + 9999999ULL) / 10000000ULL; /* ceil to 10 ms ticks */
        if (ticks == 0) ticks = 1;
        sched_sleep(ticks);

        /* If we were woken by sem_post we'll find value > 0 next iteration.
         * If we timed out, check again. */
    }
}

int posix_sem_getvalue(posix_sem_t *sem, int *sval)
{
    if (!sem || !sval) return -(int)EINVAL;
    irqflags_t fl = spinlock_lock_irqsave(&sem->lock);
    *sval = sem->value;
    spinlock_unlock_irqrestore(&sem->lock, fl);
    return 0;
}
