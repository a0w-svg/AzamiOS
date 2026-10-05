/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */
/* ============================================================================
 * AzamiOS — Inter-Process Communication Syscalls
 * File: kernel/syscall/sys_ipc.c
 * ============================================================================ */
#include "syscall_internal.h"



/* ══════════════════════════════════════════════════════════════════════════
 * Azami Extended Syscalls
 * ══════════════════════════════════════════════════════════════════════════ */

s64 sys_az_channel_create(pt_regs_t *r)
{
    (void)r;
    ipc_channel_t *chan = ipc_channel_create();
    if (!chan) return -(s64)ENOMEM;
    return (s64)chan->channel_id;
}

s64 sys_az_channel_destroy(pt_regs_t *r)
{
    u32 channel_id = (u32)r->rdi;
    ipc_channel_t *chan = ipc_channel_find(channel_id);
    if (!chan) return -(s64)EINVAL;
    ipc_channel_destroy(chan);
    ipc_channel_put(chan);
    return 0;
}

s64 sys_az_channel_send(pt_regs_t *r)
{
    u32 channel_id = (u32)r->rdi;
    const ipc_msg_t *user_msg = (const ipc_msg_t *)r->rsi;
    bool block = (bool)r->rdx;

    if (!user_msg || (uintptr_t)user_msg >= TASK_SIZE_MAX) return -(s64)EFAULT;

    ipc_channel_t *chan = ipc_channel_find(channel_id);
    if (!chan) return -(s64)EINVAL;

    ipc_msg_t kmsg;
    if (copy_from_user(&kmsg, user_msg, sizeof(ipc_msg_t)) != 0) {
        ipc_channel_put(chan);
        return -(s64)EFAULT;
    }

    s64 ret = ipc_channel_send(chan, &kmsg, block);
    ipc_channel_put(chan);
    return ret;
}

s64 sys_az_channel_recv(pt_regs_t *r)
{
    u32 channel_id = (u32)r->rdi;
    ipc_msg_t *user_msg = (ipc_msg_t *)r->rsi;
    bool block = (bool)r->rdx;

    if (!user_msg || (uintptr_t)user_msg >= TASK_SIZE_MAX) return -(s64)EFAULT;

    ipc_channel_t *chan = ipc_channel_find(channel_id);
    if (!chan) return -(s64)EINVAL;

    ipc_msg_t kmsg;
    s64 ret = ipc_channel_recv(chan, &kmsg, block);
    ipc_channel_put(chan);
    if (ret < 0) return ret;

    if (copy_to_user(user_msg, &kmsg, sizeof(ipc_msg_t)) != 0) {
        return -(s64)EFAULT;
    }
    return 0;
}

s64 sys_az_shmem_create(pt_regs_t *r)
{
    size_t page_count = (size_t)r->rdi;
    if (page_count == 0 || page_count > 4096) return -(s64)EINVAL;

    ipc_shmem_t *shmem = ipc_shmem_create(page_count);
    if (!shmem) return -(s64)ENOMEM;
    return (s64)shmem->shmem_id;
}

s64 sys_az_shmem_map(pt_regs_t *r)
{
    u32 shmem_id = (u32)r->rdi;
    virt_addr_t virt = (virt_addr_t)r->rsi;

    if (virt & (PAGE_SIZE - 1)) return -(s64)EINVAL;
    if (virt == 0 || virt >= TASK_SIZE_MAX) return -(s64)EINVAL;

    ipc_shmem_t *shmem = ipc_shmem_find(shmem_id);
    if (!shmem) return -(s64)EINVAL;

    if (shmem->page_count == 0 || shmem->page_count > 4096) {
        ipc_shmem_put(shmem);
        return -(s64)EINVAL;
    }
    if (virt + shmem->page_count * PAGE_SIZE > TASK_SIZE_MAX ||
        virt + shmem->page_count * PAGE_SIZE < virt) {
        ipc_shmem_put(shmem);
        return -(s64)EINVAL;
    }

    process_t *proc = sched_current_process();
    if (!proc) { ipc_shmem_put(shmem); return -(s64)EPERM; }

    s64 ret = ipc_shmem_map(shmem, proc, virt, VMM_USER_RW);
    ipc_shmem_put(shmem);
    return ret;
}

s64 sys_az_shmem_destroy(pt_regs_t *r)
{
    u32 shmem_id = (u32)r->rdi;
    ipc_shmem_t *shmem = ipc_shmem_find(shmem_id);
    if (!shmem) return -(s64)EINVAL;
    ipc_shmem_destroy(shmem);
    ipc_shmem_put(shmem);
    return 0;
}

s64 sys_az_shmem_unmap(pt_regs_t *r)
{
    (void)r->rdi; /* shmem_id not needed if unmapping by VA */
    virt_addr_t virt = (virt_addr_t)r->rsi;
    
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EINVAL;
    
    return ipc_shmem_unmap(NULL, proc, virt);
}

s64 sys_az_object_create(pt_regs_t *r)
{
    const char *user_name = (const char *)r->rdi;
    az_obj_type_t type = (az_obj_type_t)r->rsi;
    void *payload = (void *)r->rdx;

    char kname[64];
    __builtin_memset(kname, 0, sizeof(kname));
    if (user_name) {
        if ((uintptr_t)user_name >= TASK_SIZE_MAX) return -(s64)EFAULT;
        for (int i = 0; i < 63; i++) {
            if (copy_from_user(&kname[i], user_name + i, 1) != 0) return -(s64)EFAULT;
            if (kname[i] == '\0') break;
        }
    }

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    az_object_t *obj = az_object_create(user_name ? kname : NULL, type, payload, NULL);
    if (!obj) return -(s64)ENOMEM;

    s64 handle = az_handle_open(proc, obj);
    if (handle < 0) {
        az_object_dereference(obj);
        return handle;
    }
    return handle;
}

s64 sys_az_object_open(pt_regs_t *r)
{
    const char *user_name = (const char *)r->rdi;
    if (!user_name || (uintptr_t)user_name >= TASK_SIZE_MAX) return -(s64)EFAULT;

    char kname[64];
    __builtin_memset(kname, 0, sizeof(kname));
    for (int i = 0; i < 63; i++) {
        if (copy_from_user(&kname[i], user_name + i, 1) != 0) return -(s64)EFAULT;
        if (kname[i] == '\0') break;
    }

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    az_object_t *obj = az_object_lookup(kname);
    if (!obj) return -(s64)ENOENT;

    s64 handle = az_handle_open(proc, obj);
    az_object_dereference(obj);
    return handle;
}

s64 sys_az_object_close(pt_regs_t *r)
{
    s64 handle = (s64)r->rdi;
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    return az_handle_close(proc, handle);
}

/* ── Linux Fast Userspace Mutex (futex) ──────────────────────────────────── */
#define FUTEX_WAIT            0
#define FUTEX_WAKE            1
#define FUTEX_FD              2
#define FUTEX_REQUEUE         3
#define FUTEX_CMP_REQUEUE     4
#define FUTEX_WAKE_OP         5
#define FUTEX_LOCK_PI         6
#define FUTEX_UNLOCK_PI       7
#define FUTEX_TRYLOCK_PI      8
#define FUTEX_WAIT_BITSET     9
#define FUTEX_WAKE_BITSET     10
#define FUTEX_PRIVATE_FLAG    128
#define FUTEX_CLOCK_REALTIME  256
#define FUTEX_BITSET_MATCH_ANY 0xFFFFFFFF

/* FUTEX_WAKE_OP encoded-operation field (val3):
 *   bits 28-31 op, 24-27 cmp, 12-23 oparg (12-bit signed), 0-11 cmparg. */
#define FUTEX_OP_SET          0   /* *uaddr2 = oparg        */
#define FUTEX_OP_ADD          1   /* *uaddr2 += oparg       */
#define FUTEX_OP_OR           2   /* *uaddr2 |= oparg       */
#define FUTEX_OP_ANDN         3   /* *uaddr2 &= ~oparg      */
#define FUTEX_OP_XOR          4   /* *uaddr2 ^= oparg       */
#define FUTEX_OP_OPARG_SHIFT  8   /* oparg is (1 << oparg)  */
#define FUTEX_OP_CMP_EQ       0
#define FUTEX_OP_CMP_NE       1
#define FUTEX_OP_CMP_LT       2
#define FUTEX_OP_CMP_LE       3
#define FUTEX_OP_CMP_GT       4
#define FUTEX_OP_CMP_GE       5

typedef struct futex_q {
    thread_t        *thread;
    process_t       *proc;
    uintptr_t        uaddr;
    u32              bitset;
    struct futex_q  *next;
} futex_q_t;

#define FUTEX_HASH_SIZE 64
static futex_q_t *g_futex_table[FUTEX_HASH_SIZE];
static spinlock_t g_futex_bucket_locks[FUTEX_HASH_SIZE] = { [0 ... FUTEX_HASH_SIZE - 1] = SPINLOCK_INIT };

static void futex_lock_all_buckets(void) {
    for (u32 i = 0; i < FUTEX_HASH_SIZE; i++) {
        spinlock_lock(&g_futex_bucket_locks[i]);
    }
}

static void futex_unlock_all_buckets(void) {
    for (s32 i = FUTEX_HASH_SIZE - 1; i >= 0; i--) {
        spinlock_unlock(&g_futex_bucket_locks[i]);
    }
}

static void futex_lock_two_buckets(u32 b1, u32 b2) {
    if (b1 < b2) {
        spinlock_lock(&g_futex_bucket_locks[b1]);
        spinlock_lock(&g_futex_bucket_locks[b2]);
    } else if (b1 > b2) {
        spinlock_lock(&g_futex_bucket_locks[b2]);
        spinlock_lock(&g_futex_bucket_locks[b1]);
    } else {
        spinlock_lock(&g_futex_bucket_locks[b1]);
    }
}

static void futex_unlock_two_buckets(u32 b1, u32 b2) {
    if (b1 < b2) {
        spinlock_unlock(&g_futex_bucket_locks[b2]);
        spinlock_unlock(&g_futex_bucket_locks[b1]);
    } else if (b1 > b2) {
        spinlock_unlock(&g_futex_bucket_locks[b1]);
        spinlock_unlock(&g_futex_bucket_locks[b2]);
    } else {
        spinlock_unlock(&g_futex_bucket_locks[b1]);
    }
}

static inline u32 futex_hash(uintptr_t uaddr) {
    return (u32)((uaddr >> 2) ^ (uaddr >> 8)) % FUTEX_HASH_SIZE;
}

/* futex_wait_queued() — enqueue the calling thread on @uaddr's bucket, sleep,
 * and unwind the queue entry however the sleep ended.
 *
 * Shared by futex(FUTEX_WAIT*) and futex_wait(2); the two differ only in how
 * they arrive at @timeout_ticks (relative for the old call, absolute for the
 * new one), which is exactly the part that belongs to the caller. Returns 0 if
 * woken by a FUTEX_WAKE, -EINTR on a pending signal, -ETIMEDOUT on expiry. */
static s64 futex_wait_queued(process_t *proc, thread_t *curr, uintptr_t uaddr,
                             u32 bitset, u64 timeout_ticks)
{
    futex_q_t q;
    q.thread = curr;
    q.proc = proc;
    q.uaddr = uaddr;
    q.bitset = bitset;
    q.next = NULL;

    u32 b = futex_hash(uaddr);
    spinlock_lock(&g_futex_bucket_locks[b]);
    q.next = g_futex_table[b];
    g_futex_table[b] = &q;
    spinlock_unlock(&g_futex_bucket_locks[b]);

    u64 start_ticks = sched_get_ticks();
    if (timeout_ticks > 0) {
        sched_sleep(timeout_ticks);
    } else {
        sched_block(THREAD_BLOCKED_PENDING);
    }

    spinlock_lock(&g_futex_bucket_locks[b]);
    bool was_woken = true;
    futex_q_t **curr_q = &g_futex_table[b];
    while (*curr_q) {
        if (*curr_q == &q) {
            *curr_q = q.next;
            was_woken = false; /* Still in queue -> timed out or signal */
            break;
        }
        curr_q = &(*curr_q)->next;
    }
    spinlock_unlock(&g_futex_bucket_locks[b]);

    /* BUG-AJ fix: return -ETIMEDOUT or -EINTR when not awakened by FUTEX_WAKE */
    if (!was_woken) {
        if (proc->sig_pending & ~proc->sig_blocked)
            return -(s64)EINTR;
        if (timeout_ticks > 0 && (sched_get_ticks() - start_ticks >= timeout_ticks))
            return -(s64)ETIMEDOUT;
    }
    return 0;
}

/* futex_wake_addr() — wake up to @nr waiters queued on @uaddr in @proc whose
 * bitset intersects @bitset. Shared by futex(FUTEX_WAKE*) and by thread exit,
 * which must wake the CLONE_CHILD_CLEARTID futex without going through a
 * syscall frame. Returns the number of threads actually woken. */
static int futex_wake_addr(process_t *proc, uintptr_t uaddr, u32 nr, u32 bitset)
{
    u32 b = futex_hash(uaddr);
    int woken = 0;

    spinlock_lock(&g_futex_bucket_locks[b]);
    futex_q_t **curr_q = &g_futex_table[b];
    while (*curr_q && (u32)woken < nr) {
        futex_q_t *q = *curr_q;
        if (q->proc == proc && q->uaddr == uaddr && (q->bitset & bitset)) {
            *curr_q = q->next;
            q->next = NULL;
            sched_unblock(q->thread);
            woken++;
        } else {
            curr_q = &(*curr_q)->next;
        }
    }
    spinlock_unlock(&g_futex_bucket_locks[b]);
    return woken;
}

/* thread_clear_child_tid() — the exit half of CLONE_CHILD_CLEARTID and
 * set_tid_address(2). Zero the registered word in user memory and wake one
 * waiter on it, exactly as Linux's mm_release() does. Both halves matter: a
 * joiner that only ever sees the futex wake, with the word still holding the
 * dead tid, spins straight back into the wait. */
void thread_clear_child_tid(thread_t *t)
{
    if (!t || !t->clear_child_tid) return;

    uintptr_t uaddr = (uintptr_t)t->clear_child_tid;
    t->clear_child_tid = 0;
    if (uaddr >= TASK_SIZE_MAX || (uaddr & 3)) return;

    u32 zero = 0;
    if (copy_to_user((void *)uaddr, &zero, sizeof(zero)) != 0) return;

    futex_wake_addr(t->proc, uaddr, 1, FUTEX_BITSET_MATCH_ANY);
}

s64 sys_futex_impl(pt_regs_t *r)
{
    uintptr_t uaddr = (uintptr_t)r->rdi;
    int op = (int)r->rsi;
    u32 val = (u32)r->rdx;
    const struct linux_timespec *timeout = (const struct linux_timespec *)r->r10;
    uintptr_t uaddr2 = (uintptr_t)r->r8;
    u32 val3 = (u32)r->r9;

    if (uaddr >= TASK_SIZE_MAX || (uaddr & 3) != 0) return -(s64)EFAULT;

    int cmd = op & ~(FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME);
    process_t *proc = sched_current_process();
    thread_t *curr = sched_current_thread();
    if (!proc || !curr) return -(s64)EPERM;

    switch (cmd) {
    case FUTEX_WAIT:
    case FUTEX_WAIT_BITSET: {
        u32 bitset = (cmd == FUTEX_WAIT_BITSET) ? val3 : FUTEX_BITSET_MATCH_ANY;
        if (bitset == 0) return -(s64)EINVAL;

        u32 cur_val = 0;
        if (copy_from_user(&cur_val, (const void *)uaddr, sizeof(u32)) != 0) return -(s64)EFAULT;
        if (cur_val != val) return -(s64)11; /* -EAGAIN / -EWOULDBLOCK */

        u64 timeout_ticks = 0;
        if (timeout && (uintptr_t)timeout < TASK_SIZE_MAX) {
            struct linux_timespec ts;
            if (copy_from_user(&ts, timeout, sizeof(ts)) == 0) {
                u64 ms = (u64)ts.tv_sec * 1000 + (u64)ts.tv_nsec / 1000000;
                timeout_ticks = (ms + 9) / 10;
            }
        }

        return futex_wait_queued(proc, curr, uaddr, bitset, timeout_ticks);
    }
    case FUTEX_WAKE:
    case FUTEX_WAKE_BITSET: {
        u32 bitset = (cmd == FUTEX_WAKE_BITSET) ? val3 : FUTEX_BITSET_MATCH_ANY;
        if (bitset == 0) return -(s64)EINVAL;

        return futex_wake_addr(proc, uaddr, val, bitset);
    }
    case FUTEX_WAKE_OP: {
        /* Atomically apply an operation to *uaddr2, wake up to @val waiters on
         * uaddr, then — if oldval compares true against cmparg — wake up to
         * @val2 waiters on uaddr2. This is what glibc's pthread_cond_signal /
         * _broadcast and several bounded-queue primitives are built on. */
        if (uaddr2 >= TASK_SIZE_MAX || (uaddr2 & 3) != 0) return -(s64)EFAULT;

        u32 nr_wake  = val;
        u32 nr_wake2 = (u32)(uintptr_t)timeout;   /* val2 shares the timeout slot */
        u32 encoded  = val3;

        int wake_op  = (int)((encoded >> 28) & 0xf);
        int wake_cmp = (int)((encoded >> 24) & 0xf);
        s32 oparg    = (s32)((encoded >> 12) & 0xfff);
        s32 cmparg   = (s32)(encoded & 0xfff);
        if (oparg  & 0x800) oparg  |= ~0xfff;     /* sign-extend 12-bit fields */
        if (cmparg & 0x800) cmparg |= ~0xfff;

        if (wake_op & FUTEX_OP_OPARG_SHIFT) {
            if (oparg < 0 || oparg > 31) return -(s64)EINVAL;
            oparg = 1 << oparg;
            wake_op &= ~FUTEX_OP_OPARG_SHIFT;
        }

        u32 oldval = 0;
        if (copy_from_user(&oldval, (const void *)uaddr2, sizeof(u32)) != 0)
            return -(s64)EFAULT;

        u32 newval;
        switch (wake_op) {
        case FUTEX_OP_SET:  newval = (u32)oparg;          break;
        case FUTEX_OP_ADD:  newval = oldval + (u32)oparg; break;
        case FUTEX_OP_OR:   newval = oldval | (u32)oparg; break;
        case FUTEX_OP_ANDN: newval = oldval & ~(u32)oparg; break;
        case FUTEX_OP_XOR:  newval = oldval ^ (u32)oparg; break;
        default: return -(s64)ENOSYS;
        }

        if (newval != oldval &&
            copy_to_user((void *)uaddr2, &newval, sizeof(u32)) != 0)
            return -(s64)EFAULT;

        int woken = futex_wake_addr(proc, uaddr, nr_wake, FUTEX_BITSET_MATCH_ANY);

        int cmp_res;
        switch (wake_cmp) {
        case FUTEX_OP_CMP_EQ: cmp_res = ((s32)oldval == cmparg); break;
        case FUTEX_OP_CMP_NE: cmp_res = ((s32)oldval != cmparg); break;
        case FUTEX_OP_CMP_LT: cmp_res = ((s32)oldval <  cmparg); break;
        case FUTEX_OP_CMP_LE: cmp_res = ((s32)oldval <= cmparg); break;
        case FUTEX_OP_CMP_GT: cmp_res = ((s32)oldval >  cmparg); break;
        case FUTEX_OP_CMP_GE: cmp_res = ((s32)oldval >= cmparg); break;
        default: return -(s64)ENOSYS;
        }

        if (cmp_res)
            woken += futex_wake_addr(proc, uaddr2, nr_wake2, FUTEX_BITSET_MATCH_ANY);

        return woken;
    }
    case FUTEX_REQUEUE:
    case FUTEX_CMP_REQUEUE: {
        if (cmd == FUTEX_CMP_REQUEUE) {
            u32 cur_val = 0;
            if (copy_from_user(&cur_val, (const void *)uaddr, sizeof(u32)) != 0) return -(s64)EFAULT;
            if (cur_val != val3) return -(s64)11; /* -EAGAIN */
        }

        u32 b1 = futex_hash(uaddr);
        u32 b2 = futex_hash(uaddr2);
        int woken = 0;
        int requeued = 0;
        u32 val2_max = timeout ? (u32)(uintptr_t)timeout : 0;

        futex_lock_two_buckets(b1, b2);
        futex_q_t **curr_q = &g_futex_table[b1];
        while (*curr_q) {
            futex_q_t *entry = *curr_q;
            if (entry->proc == proc && entry->uaddr == uaddr) {
                if ((u32)woken < val) {
                    *curr_q = entry->next;
                    entry->next = NULL;
                    sched_unblock(entry->thread);
                    woken++;
                    continue;
                } else if ((u32)requeued < val2_max) {
                    /* BUG-AI fix: move requeued waiter to bucket b2 so future wakeups find it */
                    *curr_q = entry->next;
                    entry->uaddr = uaddr2;
                    entry->next = g_futex_table[b2];
                    g_futex_table[b2] = entry;
                    requeued++;
                    continue;
                }
            }
            curr_q = &(*curr_q)->next;
        }
        futex_unlock_two_buckets(b1, b2);

        return woken + requeued;
    }
    default:
        return -(s64)ENOSYS;
    }
}

/* ============================================================================
 * System V IPC (XSI) — thin marshalling over kernel/ipc/sysvipc.c
 * ========================================================================= */

s64 sys_shmget_impl(pt_regs_t *r)
{
    return sysv_shmget((s32)r->rdi, (size_t)r->rsi, (int)r->rdx);
}
s64 sys_shmat_impl(pt_regs_t *r)
{
    return sysv_shmat((int)r->rdi, (virt_addr_t)r->rsi, (int)r->rdx);
}
s64 sys_shmdt_impl(pt_regs_t *r)
{
    return sysv_shmdt((virt_addr_t)r->rdi);
}
s64 sys_shmctl_impl(pt_regs_t *r)
{
    return sysv_shmctl((int)r->rdi, (int)r->rsi, (void *)r->rdx);
}
s64 sys_semget_impl(pt_regs_t *r)
{
    return sysv_semget((s32)r->rdi, (int)r->rsi, (int)r->rdx);
}
s64 sys_semop_impl(pt_regs_t *r)
{
    return sysv_semop((int)r->rdi, (const void *)r->rsi, (size_t)r->rdx);
}
/* semtimedop(semid, sops, nsops, timeout) — semop() that gives up with EAGAIN
 * once `timeout` has elapsed.  A NULL timeout is plain semop(). */
s64 sys_semtimedop_impl(pt_regs_t *r)
{
    const struct linux_timespec *uts = (const struct linux_timespec *)r->r10;
    if (!uts) return sysv_semop((int)r->rdi, (const void *)r->rsi, (size_t)r->rdx);

    if ((uintptr_t)uts >= TASK_SIZE_MAX) return -(s64)EFAULT;
    struct linux_timespec ts;
    if (copy_from_user(&ts, uts, sizeof(ts)) != 0) return -(s64)EFAULT;
    if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000L) return -(s64)EINVAL;

    /* Ticks run at 100 Hz. A non-zero timeout must never round down to zero,
     * which would turn semtimedop() into a non-blocking poll. */
    u64 ticks = (u64)ts.tv_sec * 100 + (u64)ts.tv_nsec / 10000000ULL;
    if (ticks == 0 && (ts.tv_sec || ts.tv_nsec)) ticks = 1;

    return sysv_semtimedop((int)r->rdi, (const void *)r->rsi, (size_t)r->rdx, ticks, true);
}
s64 sys_semctl_impl(pt_regs_t *r)
{
    /* The fourth argument is glibc's `union semun` passed by value: an int for
     * SETVAL, a pointer for the rest. */
    return sysv_semctl((int)r->rdi, (int)r->rsi, (int)r->rdx, r->r10);
}
s64 sys_msgget_impl(pt_regs_t *r)
{
    return sysv_msgget((s32)r->rdi, (int)r->rsi);
}
s64 sys_msgsnd_impl(pt_regs_t *r)
{
    return sysv_msgsnd((int)r->rdi, (const void *)r->rsi, (size_t)r->rdx, (int)r->r10);
}
s64 sys_msgrcv_impl(pt_regs_t *r)
{
    return sysv_msgrcv((int)r->rdi, (void *)r->rsi, (size_t)r->rdx, (s64)r->r10, (int)r->r8);
}
s64 sys_msgctl_impl(pt_regs_t *r)
{
    return sysv_msgctl((int)r->rdi, (int)r->rsi, (void *)r->rdx);
}

/* ============================================================================
 * set_robust_list(2) / get_robust_list(2)
 * ========================================================================= */

s64 sys_set_robust_list_impl(pt_regs_t *r)
{
    void  *head = (void *)r->rdi;
    size_t len  = (size_t)r->rsi;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    /* Linux rejects a size that does not match its own struct robust_list_head,
     * which is 24 bytes on x86-64; anything else means the caller and kernel
     * disagree about the layout. */
    if (len != 24) return -(s64)EINVAL;
    if (head && (uintptr_t)head >= TASK_SIZE_MAX) return -(s64)EFAULT;

    proc->robust_list     = head;
    proc->robust_list_len = len;
    return 0;
}

s64 sys_get_robust_list_impl(pt_regs_t *r)
{
    s32     pid    = (s32)r->rdi;
    void  **uhead  = (void **)r->rsi;
    size_t *ulen   = (size_t *)r->rdx;

    process_t *caller = sched_current_process();
    if (!caller) return -(s64)EPERM;
    if (!uhead || !ulen) return -(s64)EFAULT;
    if ((uintptr_t)uhead >= TASK_SIZE_MAX ||
        (uintptr_t)ulen  >= TASK_SIZE_MAX) return -(s64)EFAULT;

    /* pid 0 is the caller (always live). Any other pid is referenced for the
     * duration so it cannot be reaped between the lookup and the field reads. */
    process_t *target = caller;
    if (pid != 0) {
        target = proc_get_by_pid((u32)pid);
        if (!target) return -(s64)ESRCH;
    }

    s64 rc = 0;
    /* Reading another process's list is a credential check, same as ptrace. */
    if (target != caller && caller->euid != 0 && caller->uid != target->uid) {
        rc = -(s64)EPERM;
    } else {
        void  *head = target->robust_list;
        size_t len  = target->robust_list_len ? target->robust_list_len : 24;
        if (copy_to_user(uhead, &head, sizeof(head)) != 0) rc = -(s64)EFAULT;
        else if (copy_to_user(ulen, &len, sizeof(len)) != 0) rc = -(s64)EFAULT;
    }

    if (target != caller) proc_put(target);
    return rc;
}

/* ============================================================================
 * futex_waitv(2) — wait until any one of several futexes is woken
 * ========================================================================= */

s64 sys_futex_waitv_impl(pt_regs_t *r)
{
    const struct futex_waitv *uwaiters = (const struct futex_waitv *)r->rdi;
    unsigned int nr = (unsigned int)r->rsi;
    unsigned int flags = (unsigned int)r->rdx;
    const struct linux_timespec *utimeout = (const struct linux_timespec *)r->r10;

    if (flags != 0) return -(s64)EINVAL;
    if (nr == 0 || nr > FUTEX_WAITV_MAX) return -(s64)EINVAL;
    if (!uwaiters || (uintptr_t)uwaiters >= TASK_SIZE_MAX) return -(s64)EFAULT;

    process_t *proc = sched_current_process();
    thread_t  *curr = sched_current_thread();
    if (!proc || !curr) return -(s64)EPERM;

    struct futex_waitv w[FUTEX_WAITV_MAX];
    if (copy_from_user(w, uwaiters, (size_t)nr * sizeof(w[0])) != 0) return -(s64)EFAULT;

    for (unsigned int i = 0; i < nr; i++) {
        if (w[i].__reserved != 0) return -(s64)EINVAL;
        /* Only 32-bit futexes exist here; the size field must say so. */
        if ((w[i].flags & 0x0F) != FUTEX2_SIZE_U32) return -(s64)EINVAL;
        if (w[i].uaddr >= TASK_SIZE_MAX || (w[i].uaddr & 3) != 0)
            return -(s64)EFAULT;
    }

    u64 timeout_ticks = 0;
    if (utimeout) {
        if ((uintptr_t)utimeout >= TASK_SIZE_MAX) return -(s64)EFAULT;
        struct linux_timespec ts;
        if (copy_from_user(&ts, utimeout, sizeof(ts)) != 0) return -(s64)EFAULT;
        if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000L)
            return -(s64)EINVAL;
        u64 ms = (u64)ts.tv_sec * 1000 + (u64)ts.tv_nsec / 1000000;
        timeout_ticks = (ms + 9) / 10;
        if (timeout_ticks == 0) timeout_ticks = 1;
    }

    /* Enqueue on every futex first, then re-check all the values. Checking
     * before enqueueing would lose a wake that lands in between. */
    futex_q_t q[FUTEX_WAITV_MAX];
    u32 bucket[FUTEX_WAITV_MAX];

    futex_lock_all_buckets();
    for (unsigned int i = 0; i < nr; i++) {
        q[i].thread = curr;
        q[i].proc   = proc;
        q[i].uaddr  = (uintptr_t)w[i].uaddr;
        q[i].bitset = FUTEX_BITSET_MATCH_ANY;
        bucket[i]   = futex_hash(q[i].uaddr);
        q[i].next   = g_futex_table[bucket[i]];
        g_futex_table[bucket[i]] = &q[i];
    }
    futex_unlock_all_buckets();

    /* dequeue_all() must run on every exit path below, including the early
     * "value already changed" one. */
    #define FUTEX_WAITV_DEQUEUE(found_out) do {                          \
        futex_lock_all_buckets();                                        \
        for (unsigned int _i = 0; _i < nr; _i++) {                       \
            futex_q_t **pp = &g_futex_table[bucket[_i]];                 \
            bool _still = false;                                         \
            while (*pp) {                                                \
                if (*pp == &q[_i]) { *pp = q[_i].next; _still = true; break; } \
                pp = &(*pp)->next;                                       \
            }                                                            \
            if (!_still) (found_out) = true;                             \
        }                                                                \
        futex_unlock_all_buckets();                                      \
    } while (0)

    for (unsigned int i = 0; i < nr; i++) {
        u32 cur = 0;
        bool unqueued = false;   /* the macro reports it; nothing to act on here */
        if (copy_from_user(&cur, (const void *)(uintptr_t)w[i].uaddr, sizeof(u32)) != 0) {
            FUTEX_WAITV_DEQUEUE(unqueued);
            (void)unqueued;
            return -(s64)EFAULT;
        }
        if (cur != (u32)w[i].val) {
            /* This futex already moved: nothing to wait for, report its index. */
            FUTEX_WAITV_DEQUEUE(unqueued);
            (void)unqueued;
            return (s64)i;
        }
    }

    u64 start_ticks = sched_get_ticks();
    if (timeout_ticks > 0) sched_sleep(timeout_ticks);
    else                   sched_block(THREAD_BLOCKED_PENDING);

    bool woken = false;
    FUTEX_WAITV_DEQUEUE(woken);
    #undef FUTEX_WAITV_DEQUEUE

    if (!woken) {
        if (proc->sig_pending & ~proc->sig_blocked) return -(s64)EINTR;
        if (timeout_ticks > 0 && sched_get_ticks() - start_ticks >= timeout_ticks)
            return -(s64)ETIMEDOUT;
        return -(s64)EINTR;
    }

    /* Report the first futex whose value now differs from what we waited on;
     * fall back to 0 when the waker changed nothing observable. */
    for (unsigned int i = 0; i < nr; i++) {
        u32 cur = 0;
        if (copy_from_user(&cur, (const void *)(uintptr_t)w[i].uaddr, sizeof(u32)) == 0 &&
            cur != (u32)w[i].val)
            return (s64)i;
    }
    return 0;
}

/* ============================================================================
 * POSIX message queues (mq_open, mq_unlink, mq_timedsend, mq_timedreceive,
 * mq_notify, mq_getsetattr)
 *
 * The subsystem lives in kernel/ipc/mqueue.c; these are register-unpacking
 * shims and nothing else, so the ABI mapping stays visible in one place.
 * ========================================================================= */

s64 sys_mq_open_impl(pt_regs_t *r)
{
    return mq_open_impl((const char *)r->rdi, (int)r->rsi, (u32)r->rdx,
                        (const void *)r->r10);
}

s64 sys_mq_unlink_impl(pt_regs_t *r)
{
    return mq_unlink_impl((const char *)r->rdi);
}

s64 sys_mq_timedsend_impl(pt_regs_t *r)
{
    return mq_timedsend_impl((int)r->rdi, (const char *)r->rsi, (size_t)r->rdx,
                             (u32)r->r10, (const void *)r->r8);
}

s64 sys_mq_timedreceive_impl(pt_regs_t *r)
{
    return mq_timedreceive_impl((int)r->rdi, (char *)r->rsi, (size_t)r->rdx,
                                (u32 *)r->r10, (const void *)r->r8);
}

s64 sys_mq_notify_impl(pt_regs_t *r)
{
    return mq_notify_impl((int)r->rdi, (const void *)r->rsi);
}

s64 sys_mq_getsetattr_impl(pt_regs_t *r)
{
    return mq_getsetattr_impl((int)r->rdi, (const void *)r->rsi, (void *)r->rdx);
}

/* ============================================================================
 * futex2: futex_wake(2), futex_wait(2), futex_requeue(2)
 *
 * The newer, flag-word-based entry points onto the same wait queues futex(2)
 * uses. They are not sugar: futex_wait() takes an *absolute* deadline against
 * a caller-named clock, which is what a correct pthread_cond_timedwait() needs
 * and what the FUTEX_WAIT opcode could never express without the
 * FUTEX_CLOCK_REALTIME retrofit.
 * ========================================================================= */

/* Reject the parts of the futex2 flag word this kernel cannot honour, so a
 * program probing for 64-bit or NUMA futexes gets a clean -EINVAL instead of
 * silently operating on the wrong width. */
static s64 futex2_check_flags(unsigned int flags)
{
    if (flags & ~(u32)(FUTEX2_SIZE_MASK | FUTEX2_NUMA | FUTEX2_PRIVATE))
        return -(s64)EINVAL;
    if ((flags & FUTEX2_SIZE_MASK) != FUTEX2_SIZE_U32) return -(s64)EINVAL;
    if (flags & FUTEX2_NUMA) return -(s64)EINVAL;   /* single memory node */
    return 0;
}

s64 sys_futex_wake_impl(pt_regs_t *r)
{
    uintptr_t uaddr = (uintptr_t)r->rdi;
    u32 mask        = (u32)r->rsi;
    int nr          = (int)r->rdx;
    unsigned int flags = (unsigned int)r->r10;

    s64 rc = futex2_check_flags(flags);
    if (rc < 0) return rc;
    if (mask == 0 || nr < 0) return -(s64)EINVAL;
    if (uaddr >= TASK_SIZE_MAX || (uaddr & 3) != 0) return -(s64)EFAULT;

    pt_regs_t sub = *r;
    sub.rsi = FUTEX_WAKE_BITSET | FUTEX_PRIVATE_FLAG;
    sub.rdx = (u64)(u32)nr;
    sub.r9  = mask;
    return sys_futex_impl(&sub);
}

s64 sys_futex_wait_impl(pt_regs_t *r)
{
    uintptr_t uaddr = (uintptr_t)r->rdi;
    u32 val         = (u32)r->rsi;
    u32 mask        = (u32)r->rdx;
    unsigned int flags = (unsigned int)r->r10;
    const struct linux_timespec *utimeout = (const struct linux_timespec *)r->r8;
    int clockid     = (int)r->r9;

    s64 rc = futex2_check_flags(flags);
    if (rc < 0) return rc;
    if (mask == 0) return -(s64)EINVAL;
    if (uaddr >= TASK_SIZE_MAX || (uaddr & 3) != 0) return -(s64)EFAULT;
    /* CLOCK_REALTIME (0) and CLOCK_MONOTONIC (1) are the only clocks the call
     * is defined for; both advance at the same rate here. */
    if (clockid != 0 && clockid != 1) return -(s64)EINVAL;

    process_t *proc = sched_current_process();
    thread_t  *curr = sched_current_thread();
    if (!proc || !curr) return -(s64)EPERM;

    u32 cur_val = 0;
    if (copy_from_user(&cur_val, (const void *)uaddr, sizeof(u32)) != 0)
        return -(s64)EFAULT;
    if (cur_val != val) return -(s64)EAGAIN;

    /* Absolute deadline, unlike FUTEX_WAIT's relative one. Reading the clock
     * here rather than in the caller is the whole point: a deadline that has
     * already passed must not block, however long the caller took to get here. */
    u64 timeout_ticks = 0;
    if (utimeout) {
        if ((uintptr_t)utimeout >= TASK_SIZE_MAX) return -(s64)EFAULT;
        struct linux_timespec ts;
        if (copy_from_user(&ts, utimeout, sizeof(ts)) != 0) return -(s64)EFAULT;
        if (ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000L) return -(s64)EINVAL;

        u64 now_ticks = sched_get_ticks();
        u64 abs_ticks = (u64)ts.tv_sec * 100 + (u64)(ts.tv_nsec / 10000000L);
        if (clockid == 0) {
            /* CLOCK_REALTIME deadlines are wall-clock; rebase onto ticks. */
            u64 now_sec = get_cached_unix_time();
            s64 delta_ms = (s64)((ts.tv_sec - (s64)now_sec) * 1000 +
                                 ts.tv_nsec / 1000000);
            if (delta_ms <= 0) return -(s64)ETIMEDOUT;
            timeout_ticks = (u64)((delta_ms + 9) / 10);
        } else {
            if (abs_ticks <= now_ticks) return -(s64)ETIMEDOUT;
            timeout_ticks = abs_ticks - now_ticks;
        }
        if (timeout_ticks == 0) timeout_ticks = 1;
    }

    return futex_wait_queued(proc, curr, uaddr, mask, timeout_ticks);
}

s64 sys_futex_requeue_impl(pt_regs_t *r)
{
    /* futex_requeue() takes its two futexes through a two-element array of
     * struct futex_waitv rather than as two bare pointers, which is what lets
     * each side carry its own flags. */
    const struct futex_waitv *uwaiters = (const struct futex_waitv *)r->rdi;
    unsigned int flags = (unsigned int)r->rsi;
    int nr_wake    = (int)r->rdx;
    int nr_requeue = (int)r->r10;

    if (flags != 0) return -(s64)EINVAL;
    if (nr_wake < 0 || nr_requeue < 0) return -(s64)EINVAL;
    if (!uwaiters || (uintptr_t)uwaiters >= TASK_SIZE_MAX)
        return -(s64)EFAULT;

    struct futex_waitv w[2];
    if (copy_from_user(w, uwaiters, sizeof(w)) != 0) return -(s64)EFAULT;

    for (int i = 0; i < 2; i++) {
        s64 rc = futex2_check_flags(w[i].flags);
        if (rc < 0) return rc;
        if (w[i].__reserved) return -(s64)EINVAL;
        if (w[i].uaddr >= TASK_SIZE_MAX || (w[i].uaddr & 3) != 0)
            return -(s64)EFAULT;
    }

    pt_regs_t sub = *r;
    sub.rdi = w[0].uaddr;
    sub.rsi = FUTEX_CMP_REQUEUE | FUTEX_PRIVATE_FLAG;
    sub.rdx = (u64)(u32)nr_wake;
    sub.r10 = (u64)(u32)nr_requeue;   /* the legacy path reads val2 from here */
    sub.r8  = w[1].uaddr;
    sub.r9  = (u32)w[0].val;          /* expected value at the source futex   */
    return sys_futex_impl(&sub);
}


/* ============================================================================
 * POSIX Named Semaphores — Azami extended syscalls 534–541
 *
 * Model: sem_open() returns a normal fd backed by a g_semfd_fops file whose
 * private_data points to the kernel posix_sem_t.  sem_post / sem_wait / ...
 * take that fd and look it up via fget() exactly like any other fd call.
 * sem_unlink() takes a name string.
 * ============================================================================ */

#include "../ipc/posix_sem.h"

static s64 semfd_release_op(inode_t *inode, file_t *filp)
{
    (void)inode;
    if (filp && filp->private_data) {
        posix_sem_put((posix_sem_t *)filp->private_data);
        filp->private_data = NULL;
    }
    return 0;
}

static file_operations_t g_semfd_fops = {
    .release = semfd_release_op,
};

/* sem_open(name, oflag, mode, value) → fd or −errno */
s64 sys_az_sem_open_impl(pt_regs_t *r)
{
    const char *uname = (const char *)r->rdi;
    int         oflag = (int)r->rsi;
    unsigned    mode  = (unsigned)r->rdx;
    unsigned    value = (unsigned)r->r10;

    char kname[POSIX_SEM_NAME_MAX];
    s64 err = copy_str_from_user(kname, uname, sizeof(kname));
    if (err < 0) return -(s64)EFAULT;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    posix_sem_t *sem = posix_sem_open_kern(kname, oflag, mode, value);
    if (!sem) return -(s64)ENOENT; /* caller interprets per oflag */

    file_t *f = (file_t *)kzalloc(sizeof(file_t));
    if (!f) { posix_sem_put(sem); return -(s64)ENOMEM; }

    f->f_op       = &g_semfd_fops;
    f->private_data = sem;
    f->f_count    = 1;
    f->f_mode     = 0600;

    s64 fd = fd_install(proc, f, 0);
    if (fd < 0) { posix_sem_put(sem); kfree(f); return -(s64)EMFILE; }
    return fd;
}

/* sem_close(fd) */
s64 sys_az_sem_close_impl(pt_regs_t *r)
{
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    /* A normal close() also works — we just provide the specific sem-close path. */
    return sys_close_impl(r);  /* reuse the standard close */
}

/* Helper: get the posix_sem_t from an fd, or return NULL (sets errno). */
static posix_sem_t *semfd_get(process_t *proc, int fd, file_t **fout)
{
    file_t *f = fget(proc, fd);
    if (!f) return NULL;
    if (f->f_op != &g_semfd_fops || !f->private_data) { fput(f); return NULL; }
    if (fout) *fout = f;
    return (posix_sem_t *)f->private_data;
}

/* sem_post(fd) */
s64 sys_az_sem_post_impl(pt_regs_t *r)
{
    int fd = (int)r->rdi;
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    file_t *f = NULL;
    posix_sem_t *sem = semfd_get(proc, fd, &f);
    if (!sem) return -(s64)EBADF;
    s64 ret = posix_sem_post(sem);
    fput(f);
    return ret;
}

/* sem_wait(fd) */
s64 sys_az_sem_wait_impl(pt_regs_t *r)
{
    int fd = (int)r->rdi;
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    file_t *f = NULL;
    posix_sem_t *sem = semfd_get(proc, fd, &f);
    if (!sem) return -(s64)EBADF;
    s64 ret = posix_sem_wait(sem);
    fput(f);
    return ret;
}

/* sem_trywait(fd) */
s64 sys_az_sem_trywait_impl(pt_regs_t *r)
{
    int fd = (int)r->rdi;
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    file_t *f = NULL;
    posix_sem_t *sem = semfd_get(proc, fd, &f);
    if (!sem) return -(s64)EBADF;
    s64 ret = posix_sem_trywait(sem);
    fput(f);
    return ret;
}

/* sem_timedwait(fd, *timespec) */
s64 sys_az_sem_timedwait_impl(pt_regs_t *r)
{
    int fd = (int)r->rdi;
    const struct linux_timespec *uts = (const struct linux_timespec *)r->rsi;
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    u64 abs_ns = 0;
    if (uts) {
        struct linux_timespec ts;
        if (copy_from_user(&ts, uts, sizeof(ts)) != 0) return -(s64)EFAULT;
        abs_ns = (u64)ts.tv_sec * 1000000000ULL + (u64)ts.tv_nsec;
    }

    file_t *f = NULL;
    posix_sem_t *sem = semfd_get(proc, fd, &f);
    if (!sem) return -(s64)EBADF;
    s64 ret = posix_sem_timedwait(sem, abs_ns);
    fput(f);
    return ret;
}

/* sem_unlink(name) */
s64 sys_az_sem_unlink_impl(pt_regs_t *r)
{
    const char *uname = (const char *)r->rdi;
    char kname[POSIX_SEM_NAME_MAX];
    s64 err = copy_str_from_user(kname, uname, sizeof(kname));
    if (err < 0) return -(s64)EFAULT;
    return (s64)posix_sem_unlink(kname);
}

/* sem_getvalue(fd, *sval) */
s64 sys_az_sem_getvalue_impl(pt_regs_t *r)
{
    int  fd   = (int)r->rdi;
    int *usval = (int *)r->rsi;
    if (!usval || (uintptr_t)usval >= TASK_SIZE_MAX) return -(s64)EFAULT;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    file_t *f = NULL;
    posix_sem_t *sem = semfd_get(proc, fd, &f);
    if (!sem) return -(s64)EBADF;
    int sval = 0;
    s64 ret = posix_sem_getvalue(sem, &sval);
    fput(f);
    if (ret == 0 && copy_to_user(usval, &sval, sizeof(sval)) != 0)
        return -(s64)EFAULT;
    return ret;
}
