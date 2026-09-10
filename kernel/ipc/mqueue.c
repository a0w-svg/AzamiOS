/* ============================================================================
 * AzamiOS — POSIX Message Queues
 * File: kernel/ipc/mqueue.c
 *
 * See mqueue.h for the interface and for why this is not the System V queue
 * code wearing a different hat.
 *
 * Structure of the implementation:
 *
 *   - A fixed table of queues, keyed by name. Names live in the table; the
 *     lifetime of the *queue* is a reference count, so mq_unlink() detaching
 *     the name never invalidates a descriptor someone still holds.
 *   - Each descriptor is an ordinary file_t with its own file_operations, so
 *     poll/select/epoll, close-on-exec, fork inheritance and the fd table's
 *     reference counting all come for free and behave the way a program that
 *     learnt them on Linux expects.
 *   - Messages hang off the queue in priority order, highest first, FIFO
 *     within a priority — that ordering *is* the interface, so it is enforced
 *     at insert rather than at receive.
 *
 * Blocking follows the pattern the System V code established: drop the lock,
 * sleep a tick, re-validate everything from scratch on wake. It costs a tick
 * of latency against a proper wait queue and in exchange cannot leak a waiter
 * when the queue it was waiting on is removed underneath it.
 * ============================================================================ */

#include "mqueue.h"
#include "../sched/sched.h"
#include "../ktimer.h"
#include "../mm/kmalloc.h"
#include "../lib/string.h"
#include "../uaccess.h"
#include "../../include/azami/defs.h"
#include "../../arch/x86_64/cpu/spinlock.h"

extern u64  get_cached_unix_time(void);
extern int  scnprintf(char *buf, size_t size, const char *fmt, ...);

/* fd-table plumbing owned by the syscall layer. */
extern s64      syscall_fd_install(process_t *proc, file_t *file, u8 fd_flags);
extern file_t  *syscall_fget(process_t *proc, int fd);
extern void     syscall_fput(file_t *file);

/* poll(2) revents. Defined per-module throughout this kernel rather than in a
 * shared header; keep these in step with kernel/syscall/syscall.c. */
#define POLLIN      0x0001
#define POLLOUT     0x0004
#define POLLNVAL    0x0020
#define POLLRDNORM  0x0040
#define POLLWRNORM  0x0100

/* ── Types ───────────────────────────────────────────────────────────────── */

typedef struct mq_msg {
    struct mq_msg *next;
    u32            prio;
    size_t         len;
    u8             data[];
} mq_msg_t;

typedef struct {
    bool      used;
    char      name[MQ_NAME_MAX + 1];  /* stored without the leading '/' */
    u32       refs;                   /* open descriptors, +1 while linked */
    bool      linked;                 /* name still resolvable            */

    u32       uid, gid, mode;
    s64       maxmsg, msgsize;
    s64       curmsgs;
    size_t    curbytes;

    mq_msg_t *head;                   /* highest priority first           */
    mq_msg_t *tail;

    /* One-shot async notification (mq_notify). */
    u32       notify_pid;             /* 0 when nobody is registered      */
    int       notify_signo;
    int       notify_type;            /* SIGEV_*                          */

    u32       recv_waiters;           /* readers currently blocked        */
    s64       ctime;
} mq_queue_t;

static mq_queue_t g_mq[MQ_MAX_QUEUES];
static spinlock_t g_mq_lock = SPINLOCK_INIT;

/* ── Small helpers ───────────────────────────────────────────────────────── */

static bool user_ptr_ok(const void *p)
{
    return p != NULL && (uintptr_t)p < 0x0000800000000000ULL;
}

/* POSIX names are "/name" with no further slashes; anything else is a name
 * this implementation is required to reject rather than reinterpret. */
static s64 mq_copy_name(const char *uname, char *out)
{
    if (!user_ptr_ok(uname)) return -(s64)EFAULT;

    /* '/' + at most MQ_NAME_MAX characters + NUL. Sizing it exactly is what
     * turns an over-long name into ENAMETOOLONG instead of a silent truncation
     * that would alias two different queues onto one name. */
    char raw[MQ_NAME_MAX + 2];
    for (size_t i = 0; i < sizeof(raw); i++) {
        if (copy_from_user(&raw[i], uname + i, 1) != 0) return -(s64)EFAULT;
        if (raw[i] == '\0') {
            if (i < 2) return -(s64)EINVAL;          /* "" or "/" alone */
            if (raw[0] != '/') return -(s64)EINVAL;
            for (size_t k = 1; k < i; k++)
                if (raw[k] == '/') return -(s64)EINVAL;
            memcpy(out, raw + 1, i);                 /* includes the NUL */
            return 0;
        }
    }
    return -(s64)ENAMETOOLONG;
}

static mq_queue_t *mq_find(const char *name)
{
    for (int i = 0; i < MQ_MAX_QUEUES; i++)
        if (g_mq[i].used && g_mq[i].linked && strcmp(g_mq[i].name, name) == 0)
            return &g_mq[i];
    return NULL;
}

/* Access check against the queue's mode bits. @want is the r/w pair the caller
 * needs, as 4 (read) and/or 2 (write) in the "other" position. */
static bool mq_permitted(const mq_queue_t *q, const process_t *p, u32 want)
{
    if (p->euid == 0) return true;
    u32 mode = q->mode;
    if (p->euid == q->uid)      mode >>= 6;
    else if (p->egid == q->gid) mode >>= 3;
    else {
        for (u32 i = 0; i < p->ngroups; i++) {
            if (p->groups[i] == q->gid) { mode >>= 3; goto check; }
        }
    }
check:
    return (mode & want) == want;
}

/* Release one reference. Frees the queue (and any messages still in it) once
 * the name is gone and the last descriptor has closed.
 * Must be called with g_mq_lock held; may drop and retake it to free. */
static void mq_put_locked(mq_queue_t *q)
{
    if (!q || q->refs == 0) return;
    if (--q->refs > 0 || q->linked) return;

    mq_msg_t *m = q->head;
    q->head = q->tail = NULL;
    q->used = false;
    memset(q->name, 0, sizeof(q->name));

    /* kfree() outside the lock: the allocator takes its own, and holding two
     * locks in an order nothing else follows is how deadlocks are built. */
    spinlock_unlock(&g_mq_lock);
    while (m) { mq_msg_t *n = m->next; kfree(m); m = n; }
    spinlock_lock(&g_mq_lock);
}

/* Absolute CLOCK_REALTIME deadline -> absolute scheduler tick, matching the
 * conversion the futex and System V paths already use (1 tick = 10 ms).
 * Returns 0 and sets *expired when the deadline is already in the past. */
struct k_timespec { s64 tv_sec; s64 tv_nsec; };

static s64 mq_deadline_ticks(const void *uts, bool *has_timeout, u64 *deadline,
                             bool *expired)
{
    *has_timeout = false;
    *deadline = 0;
    *expired = false;
    if (!uts) return 0;
    if (!user_ptr_ok(uts)) return -(s64)EFAULT;

    struct k_timespec ts;
    if (copy_from_user(&ts, uts, sizeof(ts)) != 0) return -(s64)EFAULT;
    if (ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000L) return -(s64)EINVAL;

    s64 now_sec = (s64)get_cached_unix_time();
    s64 delta_ms = (ts.tv_sec - now_sec) * 1000 + ts.tv_nsec / 1000000;

    *has_timeout = true;
    if (delta_ms <= 0) { *expired = true; return 0; }
    *deadline = sched_get_ticks() + (u64)((delta_ms + 9) / 10);
    return 0;
}

/* Fire and consume the one-shot notification, if the transition warrants it.
 * Called with the lock held; the caller sends the signal after dropping it. */
static void mq_take_notify(mq_queue_t *q, bool was_empty, u32 *out_pid, int *out_signo)
{
    *out_pid = 0;
    *out_signo = 0;
    /* POSIX: notify only on the empty -> non-empty transition, only when
     * someone is registered, and only when no reader is already blocked — a
     * blocked reader will take the message, so waking a third party as well
     * would be a wakeup with nothing behind it. */
    if (!was_empty || q->notify_pid == 0 || q->recv_waiters != 0) return;
    if (q->notify_type != SIGEV_NONE && q->notify_signo) {
        *out_pid = q->notify_pid;
        *out_signo = q->notify_signo;
    }
    q->notify_pid = 0;
    q->notify_signo = 0;
    q->notify_type = SIGEV_NONE;
}

/* ── File operations ─────────────────────────────────────────────────────── */

static file_operations_t g_mq_fops;   /* defined below; referenced by mq_get */

/* Resolve an mqd_t. Returns the queue with a reference on the *file* taken,
 * which the caller releases with syscall_fput(). */
static s64 mq_from_fd(process_t *proc, int mqdes, file_t **out_f, mq_queue_t **out_q)
{
    file_t *f = syscall_fget(proc, mqdes);
    if (!f) return -(s64)EBADF;
    if (f->f_op != &g_mq_fops || !f->private_data) {
        syscall_fput(f);
        return -(s64)EBADF;
    }
    *out_f = f;
    *out_q = (mq_queue_t *)f->private_data;
    return 0;
}

/* read() on an mqd_t reports the queue's state, exactly as Linux's mqueue
 * filesystem does — a program can cat its own descriptor to see the backlog. */
static s64 mq_read_op(file_t *filp, void *buf, size_t len, u64 *offset)
{
    mq_queue_t *q = (mq_queue_t *)filp->private_data;
    if (!q) return -(s64)EBADF;

    char line[96];
    spinlock_lock(&g_mq_lock);
    int n = scnprintf(line, sizeof(line),
                      "QSIZE:%-10llu NOTIFY:%-5d SIGNO:%-5d NOTIFY_PID:%-6u\n",
                      (unsigned long long)q->curbytes,
                      q->notify_pid ? q->notify_type : 0,
                      q->notify_pid ? q->notify_signo : 0,
                      q->notify_pid);
    spinlock_unlock(&g_mq_lock);

    u64 pos = offset ? *offset : 0;
    if (pos >= (u64)n) return 0;
    size_t avail = (size_t)n - (size_t)pos;
    if (avail > len) avail = len;
    memcpy(buf, line + pos, avail);
    if (offset) *offset = pos + avail;
    return (s64)avail;
}

static int mq_poll_op(file_t *filp)
{
    mq_queue_t *q = (mq_queue_t *)filp->private_data;
    if (!q) return POLLNVAL;
    int rev = 0;
    spinlock_lock(&g_mq_lock);
    if (q->curmsgs > 0)          rev |= (POLLIN  | POLLRDNORM);
    if (q->curmsgs < q->maxmsg)  rev |= (POLLOUT | POLLWRNORM);
    spinlock_unlock(&g_mq_lock);
    return rev;
}

static s64 mq_release_op(inode_t *inode, file_t *filp)
{
    (void)inode;
    mq_queue_t *q = filp ? (mq_queue_t *)filp->private_data : NULL;
    if (!q) return 0;
    filp->private_data = NULL;

    spinlock_lock(&g_mq_lock);
    /* A process that closes the descriptor it registered on stops being
     * notified: the registration is a property of the open description. */
    process_t *proc = sched_current_process();
    if (proc && q->notify_pid == proc->pid) {
        q->notify_pid = 0;
        q->notify_signo = 0;
        q->notify_type = SIGEV_NONE;
    }
    mq_put_locked(q);
    spinlock_unlock(&g_mq_lock);
    return 0;
}

static file_operations_t g_mq_fops = {
    .read    = mq_read_op,
    .poll    = mq_poll_op,
    .release = mq_release_op,
};

/* ── mq_open / mq_unlink ─────────────────────────────────────────────────── */

void mqueue_init(void)
{
    memset(g_mq, 0, sizeof(g_mq));
    spinlock_init(&g_mq_lock);
}

/* Roll an mq_open() back after the descriptor could not be built. A queue this
 * call created must lose its name as well as its reference: leaving it linked
 * would publish a queue no descriptor was ever returned for, and the next
 * mq_open(O_CREAT|O_EXCL) on that name would fail with EEXIST forever. */
static void mq_open_undo(mq_queue_t *q, bool created)
{
    spinlock_lock(&g_mq_lock);
    if (created) {
        q->linked = false;
        mq_put_locked(q);      /* the name's reference */
    }
    mq_put_locked(q);          /* this call's reference */
    spinlock_unlock(&g_mq_lock);
}

s64 mq_open_impl(const char *uname, int oflag, u32 mode, const void *uattr)
{
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    char name[MQ_NAME_MAX + 1];
    s64 rc = mq_copy_name(uname, name);
    if (rc < 0) return rc;

    /* Access mode drives the permission check; O_CREAT/O_EXCL/O_NONBLOCK/
     * O_CLOEXEC are the only other bits POSIX gives meaning to here. */
    int accmode = oflag & 3;
    if (accmode == 3) return -(s64)EINVAL;
    u32 want = 0;
    if (accmode == O_RDONLY || accmode == O_RDWR) want |= 4;
    if (accmode == O_WRONLY || accmode == O_RDWR) want |= 2;

    struct mq_attr attr = {
        .mq_flags = 0,
        .mq_maxmsg = MQ_MAXMSG_DEFAULT,
        .mq_msgsize = MQ_MSGSIZE_DEFAULT,
        .mq_curmsgs = 0,
    };
    if ((oflag & O_CREAT) && uattr) {
        if (!user_ptr_ok(uattr)) return -(s64)EFAULT;
        if (copy_from_user(&attr, uattr, sizeof(attr)) != 0) return -(s64)EFAULT;
        if (attr.mq_maxmsg <= 0 || attr.mq_msgsize <= 0) return -(s64)EINVAL;
        if (attr.mq_maxmsg > MQ_MAXMSG_LIMIT ||
            attr.mq_msgsize > MQ_MSGSIZE_LIMIT) return -(s64)EINVAL;
        /* A caller may not reserve unbounded kernel memory through an
         * otherwise valid attribute pair. */
        if ((u64)attr.mq_maxmsg * (u64)attr.mq_msgsize > MQ_QUEUE_BYTES_MAX)
            return -(s64)EINVAL;
    }

    bool created = false;

    spinlock_lock(&g_mq_lock);

    mq_queue_t *q = mq_find(name);
    if (q) {
        if ((oflag & O_CREAT) && (oflag & O_EXCL)) {
            spinlock_unlock(&g_mq_lock);
            return -(s64)EEXIST;
        }
        if (!mq_permitted(q, proc, want)) {
            spinlock_unlock(&g_mq_lock);
            return -(s64)EACCES;
        }
        q->refs++;
    } else {
        if (!(oflag & O_CREAT)) {
            spinlock_unlock(&g_mq_lock);
            return -(s64)ENOENT;
        }
        int slot = -1;
        for (int i = 0; i < MQ_MAX_QUEUES; i++)
            if (!g_mq[i].used) { slot = i; break; }
        if (slot < 0) {
            spinlock_unlock(&g_mq_lock);
            return -(s64)ENOSPC;
        }
        created = true;
        q = &g_mq[slot];
        memset(q, 0, sizeof(*q));
        strncpy(q->name, name, MQ_NAME_MAX);
        q->used    = true;
        q->linked  = true;
        q->refs    = 2;                      /* the name, plus this descriptor */
        q->uid     = proc->euid;
        q->gid     = proc->egid;
        q->mode    = mode & ~proc->umask & 0777;
        q->maxmsg  = attr.mq_maxmsg;
        q->msgsize = attr.mq_msgsize;
        q->notify_type = SIGEV_NONE;
        q->ctime   = (s64)get_cached_unix_time();
    }

    spinlock_unlock(&g_mq_lock);

    file_t *f = (file_t *)kzalloc(sizeof(file_t));
    if (!f) {
        mq_open_undo(q, created);
        return -(s64)ENOMEM;
    }
    f->f_op         = &g_mq_fops;
    f->private_data = q;
    f->f_flags      = (u32)(oflag & (3 | O_NONBLOCK));
    f->f_fd_flags   = (oflag & O_CLOEXEC) ? FD_CLOEXEC : 0;
    f->f_mode       = q->mode;
    f->f_count      = 1;

    s64 fd = syscall_fd_install(proc, f, (u8)f->f_fd_flags);
    if (fd < 0) {
        kfree(f);
        mq_open_undo(q, created);
        return fd;
    }
    return fd;
}

s64 mq_unlink_impl(const char *uname)
{
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    char name[MQ_NAME_MAX + 1];
    s64 rc = mq_copy_name(uname, name);
    if (rc < 0) return rc;

    spinlock_lock(&g_mq_lock);
    mq_queue_t *q = mq_find(name);
    if (!q) {
        spinlock_unlock(&g_mq_lock);
        return -(s64)ENOENT;
    }
    /* Unlink needs write permission on the queue, matching Linux. */
    if (!mq_permitted(q, proc, 2)) {
        spinlock_unlock(&g_mq_lock);
        return -(s64)EACCES;
    }
    q->linked = false;
    mq_put_locked(q);                 /* drops the name's reference */
    spinlock_unlock(&g_mq_lock);
    return 0;
}

/* ── send / receive ──────────────────────────────────────────────────────── */

s64 mq_timedsend_impl(int mqdes, const char *umsg, size_t len, u32 prio,
                      const void *abs_timeout)
{
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    if (prio >= MQ_PRIO_MAX) return -(s64)EINVAL;
    if (len && !user_ptr_ok(umsg)) return -(s64)EFAULT;

    file_t *f = NULL; mq_queue_t *q = NULL;
    s64 rc = mq_from_fd(proc, mqdes, &f, &q);
    if (rc < 0) return rc;

    int accmode = (int)(f->f_flags & 3);
    if (accmode != O_WRONLY && accmode != O_RDWR) { syscall_fput(f); return -(s64)EBADF; }
    if ((s64)len > q->msgsize) { syscall_fput(f); return -(s64)EMSGSIZE; }

    bool has_timeout = false, expired = false;
    u64 deadline = 0;
    rc = mq_deadline_ticks(abs_timeout, &has_timeout, &deadline, &expired);
    if (rc < 0) { syscall_fput(f); return rc; }

    /* Copy in before taking the lock: copy_from_user() can fault and a fault
     * is resolved by code that may itself need to allocate. */
    mq_msg_t *m = (mq_msg_t *)kmalloc(sizeof(mq_msg_t) + len);
    if (!m) { syscall_fput(f); return -(s64)ENOMEM; }
    m->next = NULL;
    m->prio = prio;
    m->len  = len;
    if (len && copy_from_user(m->data, umsg, len) != 0) {
        kfree(m);
        syscall_fput(f);
        return -(s64)EFAULT;
    }

    for (;;) {
        spinlock_lock(&g_mq_lock);

        if (!q->used) {                       /* unlinked and torn down */
            spinlock_unlock(&g_mq_lock);
            kfree(m);
            syscall_fput(f);
            return -(s64)EBADF;
        }

        if (q->curmsgs < q->maxmsg) {
            bool was_empty = (q->curmsgs == 0);
            /* Priority order, FIFO within a priority. The tail shortcut makes
             * the overwhelmingly common single-priority case O(1); the walk
             * only runs when priorities are actually mixed. */
            if (!q->head) {
                q->head = q->tail = m;
            } else if (q->tail->prio >= prio) {
                q->tail->next = m;
                q->tail = m;
            } else if (q->head->prio < prio) {
                m->next = q->head;
                q->head = m;
            } else {
                mq_msg_t *prev = q->head;
                while (prev->next && prev->next->prio >= prio) prev = prev->next;
                m->next = prev->next;
                prev->next = m;
                if (!m->next) q->tail = m;
            }
            q->curmsgs++;
            q->curbytes += len;

            u32 npid; int nsig;
            mq_take_notify(q, was_empty, &npid, &nsig);
            spinlock_unlock(&g_mq_lock);

            if (npid) sched_kill_process(npid, nsig);
            syscall_fput(f);
            return 0;
        }

        spinlock_unlock(&g_mq_lock);

        if (f->f_flags & O_NONBLOCK) { kfree(m); syscall_fput(f); return -(s64)EAGAIN; }
        if (expired || (has_timeout && (s64)(sched_get_ticks() - deadline) >= 0)) {
            kfree(m);
            syscall_fput(f);
            return -(s64)ETIMEDOUT;
        }

        sched_sleep(1);
        if (proc->sig_pending & ~proc->sig_blocked) {
            kfree(m);
            syscall_fput(f);
            return -(s64)EINTR;
        }
    }
}

s64 mq_timedreceive_impl(int mqdes, char *umsg, size_t len, u32 *uprio,
                         const void *abs_timeout)
{
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    if (!user_ptr_ok(umsg)) return -(s64)EFAULT;
    if (uprio && !user_ptr_ok(uprio)) return -(s64)EFAULT;

    file_t *f = NULL; mq_queue_t *q = NULL;
    s64 rc = mq_from_fd(proc, mqdes, &f, &q);
    if (rc < 0) return rc;

    int accmode = (int)(f->f_flags & 3);
    if (accmode != O_RDONLY && accmode != O_RDWR) { syscall_fput(f); return -(s64)EBADF; }
    /* POSIX: the buffer must be able to hold *any* message the queue accepts,
     * because a message is never delivered truncated. */
    if ((s64)len < q->msgsize) { syscall_fput(f); return -(s64)EMSGSIZE; }

    bool has_timeout = false, expired = false;
    u64 deadline = 0;
    rc = mq_deadline_ticks(abs_timeout, &has_timeout, &deadline, &expired);
    if (rc < 0) { syscall_fput(f); return rc; }

    for (;;) {
        spinlock_lock(&g_mq_lock);

        if (!q->used) {
            spinlock_unlock(&g_mq_lock);
            syscall_fput(f);
            return -(s64)EBADF;
        }

        mq_msg_t *m = q->head;
        if (m) {
            q->head = m->next;
            if (!q->head) q->tail = NULL;
            q->curmsgs--;
            q->curbytes -= m->len;
            spinlock_unlock(&g_mq_lock);

            size_t mlen = m->len;
            u32 mprio = m->prio;
            s64 out = (s64)mlen;
            if (mlen && copy_to_user(umsg, m->data, mlen) != 0) out = -(s64)EFAULT;
            if (out >= 0 && uprio && copy_to_user(uprio, &mprio, sizeof(u32)) != 0)
                out = -(s64)EFAULT;
            kfree(m);
            syscall_fput(f);
            return out;
        }

        if (f->f_flags & O_NONBLOCK) {
            spinlock_unlock(&g_mq_lock);
            syscall_fput(f);
            return -(s64)EAGAIN;
        }
        if (expired || (has_timeout && (s64)(sched_get_ticks() - deadline) >= 0)) {
            spinlock_unlock(&g_mq_lock);
            syscall_fput(f);
            return -(s64)ETIMEDOUT;
        }

        /* Publishing the wait is what suppresses a redundant mq_notify signal:
         * a message arriving now has a reader for it already. */
        q->recv_waiters++;
        spinlock_unlock(&g_mq_lock);

        sched_sleep(1);

        spinlock_lock(&g_mq_lock);
        if (q->recv_waiters) q->recv_waiters--;
        spinlock_unlock(&g_mq_lock);

        if (proc->sig_pending & ~proc->sig_blocked) {
            syscall_fput(f);
            return -(s64)EINTR;
        }
    }
}

/* ── notify / getsetattr ─────────────────────────────────────────────────── */

/* The leading fields of struct sigevent, which is all the kernel reads. */
struct k_sigevent {
    u64 sigev_value;
    s32 sigev_signo;
    s32 sigev_notify;
};

s64 mq_notify_impl(int mqdes, const void *usev)
{
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    file_t *f = NULL; mq_queue_t *q = NULL;
    s64 rc = mq_from_fd(proc, mqdes, &f, &q);
    if (rc < 0) return rc;

    struct k_sigevent sev = { 0, 0, SIGEV_NONE };
    if (usev) {
        if (!user_ptr_ok(usev)) { syscall_fput(f); return -(s64)EFAULT; }
        if (copy_from_user(&sev, usev, sizeof(sev)) != 0) {
            syscall_fput(f);
            return -(s64)EFAULT;
        }
        if (sev.sigev_notify != SIGEV_SIGNAL && sev.sigev_notify != SIGEV_NONE) {
            /* SIGEV_THREAD is implemented by libc on top of a helper thread and
             * a SIGEV_SIGNAL registration; the kernel never sees it directly. */
            syscall_fput(f);
            return -(s64)EINVAL;
        }
        if (sev.sigev_notify == SIGEV_SIGNAL &&
            (sev.sigev_signo < 1 || sev.sigev_signo >= 64)) {
            syscall_fput(f);
            return -(s64)EINVAL;
        }
    }

    spinlock_lock(&g_mq_lock);
    if (!usev) {
        /* Deregister — but only our own registration. */
        if (q->notify_pid == proc->pid) {
            q->notify_pid = 0;
            q->notify_signo = 0;
            q->notify_type = SIGEV_NONE;
        }
        spinlock_unlock(&g_mq_lock);
        syscall_fput(f);
        return 0;
    }

    if (q->notify_pid != 0 && q->notify_pid != proc->pid) {
        spinlock_unlock(&g_mq_lock);
        syscall_fput(f);
        return -(s64)EBUSY;
    }
    q->notify_pid   = proc->pid;
    q->notify_type  = sev.sigev_notify;
    q->notify_signo = (sev.sigev_notify == SIGEV_NONE) ? 0 : sev.sigev_signo;
    spinlock_unlock(&g_mq_lock);

    syscall_fput(f);
    return 0;
}

s64 mq_getsetattr_impl(int mqdes, const void *unewattr, void *uoldattr)
{
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    file_t *f = NULL; mq_queue_t *q = NULL;
    s64 rc = mq_from_fd(proc, mqdes, &f, &q);
    if (rc < 0) return rc;

    struct mq_attr newattr;
    if (unewattr) {
        if (!user_ptr_ok(unewattr)) { syscall_fput(f); return -(s64)EFAULT; }
        if (copy_from_user(&newattr, unewattr, sizeof(newattr)) != 0) {
            syscall_fput(f);
            return -(s64)EFAULT;
        }
        /* O_NONBLOCK is the only settable bit; POSIX says the rest of the
         * structure is ignored rather than validated. */
        if (newattr.mq_flags & ~(s64)O_NONBLOCK) { syscall_fput(f); return -(s64)EINVAL; }
    }

    struct mq_attr old;
    spinlock_lock(&g_mq_lock);
    old.mq_flags   = (f->f_flags & O_NONBLOCK) ? O_NONBLOCK : 0;
    old.mq_maxmsg  = q->maxmsg;
    old.mq_msgsize = q->msgsize;
    old.mq_curmsgs = q->curmsgs;
    memset(old.__reserved, 0, sizeof(old.__reserved));
    if (unewattr) {
        if (newattr.mq_flags & O_NONBLOCK) f->f_flags |= O_NONBLOCK;
        else                               f->f_flags &= ~(u32)O_NONBLOCK;
    }
    spinlock_unlock(&g_mq_lock);

    if (uoldattr) {
        if (!user_ptr_ok(uoldattr)) { syscall_fput(f); return -(s64)EFAULT; }
        if (copy_to_user(uoldattr, &old, sizeof(old)) != 0) {
            syscall_fput(f);
            return -(s64)EFAULT;
        }
    }
    syscall_fput(f);
    return 0;
}

/* ── Teardown & reporting ────────────────────────────────────────────────── */

void mqueue_drop_proc(u32 pid)
{
    if (!pid) return;
    spinlock_lock(&g_mq_lock);
    for (int i = 0; i < MQ_MAX_QUEUES; i++) {
        if (g_mq[i].used && g_mq[i].notify_pid == pid) {
            g_mq[i].notify_pid = 0;
            g_mq[i].notify_signo = 0;
            g_mq[i].notify_type = SIGEV_NONE;
        }
    }
    spinlock_unlock(&g_mq_lock);
}

size_t mqueue_format_proc(char *buf, size_t max)
{
    size_t off = 0;
    spinlock_lock(&g_mq_lock);
    for (int i = 0; i < MQ_MAX_QUEUES; i++) {
        mq_queue_t *q = &g_mq[i];
        if (!q->used || !q->linked) continue;
        off += (size_t)scnprintf(buf + off, max > off ? max - off : 0,
                 "/%-20s size=%-8llu msgs=%-6lld maxmsg=%-6lld msgsize=%-8lld "
                 "mode=%04o uid=%u gid=%u refs=%u\n",
                 q->name, (unsigned long long)q->curbytes,
                 (long long)q->curmsgs, (long long)q->maxmsg,
                 (long long)q->msgsize, q->mode, q->uid, q->gid, q->refs);
    }
    spinlock_unlock(&g_mq_lock);
    return off;
}
