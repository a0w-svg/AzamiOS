/* ============================================================================
 * AzamiOS — fanotify(7)
 * File: fs/fanotify.c
 *
 * This file existed before but nothing could reach it: fanotify_init(2) and
 * fanotify_mark(2) were never registered in the syscall table, so every
 * entry point below was dead code. What was here also could not have worked
 * had it been reachable —
 *
 *   - fanotify_mark() ignored `pathname` entirely and only accepted a mark
 *     on an already-open dirfd, which is not how any program uses it;
 *   - a blocking read() parked the thread with sched_block() and nothing
 *     ever called sched_unblock(), so the first read on an empty queue hung
 *     the caller forever;
 *   - read() used copy_to_user() on `buf`, which the file_operations
 *     contract in fs/vfs.h documents as a *kernel* pointer — that call fails
 *     by construction and the read returned -EFAULT every time;
 *   - marks could only be added, never removed or flushed;
 *   - every event reported FAN_NOFD, so a listener had nothing to open.
 *
 * All of that is fixed here. Marks are taken by path, a queued event carries
 * an open file description that read() installs as a descriptor in the
 * *reading* process (not in whoever happened to trigger the event), blocked
 * readers are woken, and FAN_MARK_REMOVE/FAN_MARK_FLUSH work.
 * ============================================================================ */

#include "vfs.h"
#include "../kernel/sched/sched.h"
#include "../arch/x86_64/cpu/spinlock.h"
#include "../include/azami/uapi/fanotify.h"
#include "../include/azami/uapi/syscall_nr.h"
#include "../kernel/mm/kmalloc.h"
#include "../kernel/lib/string.h"
#include "../kernel/syscall/syscall.h"

#define POLLIN     0x0001

/* Bound on a context's queue unless it asked for FAN_UNLIMITED_QUEUE. Past
 * it, one FAN_Q_OVERFLOW event is queued and further events are dropped —
 * the same contract Linux offers, and the reason a listener is expected to
 * check for that bit. */
#define FAN_DEFAULT_MAX_QUEUE  16384

typedef struct fanotify_event_node {
    struct fanotify_event_metadata meta;
    /* The open file description the event refers to, or NULL for an event
     * with no fd (FAN_Q_OVERFLOW, or a context created FAN_REPORT_FID).
     * It is installed into the reader's fd table by fanotify_read(). */
    file_t *file;
    struct fanotify_event_node *next;
} fanotify_event_node_t;

typedef struct fanotify_context {
    spinlock_t lock;
    fanotify_event_node_t *event_queue_head;
    fanotify_event_node_t *event_queue_tail;
    u32 queue_len;
    u32 queue_max;
    u32 flags;
    u32 event_f_flags;
    bool overflowed;
    /* Threads parked in fanotify_read() on this context, linked through
     * thread_t::sem_next — the same list link fs/pipe.c uses for its own
     * reader/writer queues. */
    thread_t *readers;
    /* Every mark this context holds, so FAN_MARK_FLUSH and closing the
     * context can find them without walking every inode in the system. */
    struct fanotify_mark *marks;
} fanotify_context_t;

typedef struct fanotify_mark {
    fanotify_context_t *ctx;
    inode_t *inode;           /* what this mark is attached to */
    u64 mask;                 /* events the listener asked for */
    u64 ignored_mask;         /* events it explicitly does not want */
    char path[256];           /* resolved at mark time, used to open the fd */
    struct fanotify_mark *next;      /* next mark on the same inode */
    struct fanotify_mark *ctx_next;  /* next mark held by the same context */
} fanotify_mark_t;

/* ── reader wait queue ───────────────────────────────────────────────────── */

static void fan_wait_push(thread_t **head, thread_t *t)
{
    t->sem_next = NULL;
    if (!*head) { *head = t; return; }
    thread_t *c = *head;
    while (c->sem_next) {
        if (c == t) return;
        c = c->sem_next;
    }
    if (c != t) c->sem_next = t;
}

static thread_t *fan_wait_pop(thread_t **head)
{
    thread_t *t = *head;
    if (t) { *head = t->sem_next; t->sem_next = NULL; }
    return t;
}

static void fan_wait_remove(thread_t **head, thread_t *t)
{
    thread_t **c = head;
    while (*c) {
        if (*c == t) { *c = t->sem_next; t->sem_next = NULL; return; }
        c = &(*c)->sem_next;
    }
}

/* ── file operations ─────────────────────────────────────────────────────── */

static const file_operations_t fanotify_fops;

/* Detach the head of the queue, or NULL. Caller holds ctx->lock. */
static fanotify_event_node_t *fan_dequeue(fanotify_context_t *ctx)
{
    fanotify_event_node_t *node = ctx->event_queue_head;
    if (!node) return NULL;
    ctx->event_queue_head = node->next;
    if (!ctx->event_queue_head) ctx->event_queue_tail = NULL;
    ctx->queue_len--;
    node->next = NULL;
    return node;
}

/*
 * read(2) on a fanotify descriptor: as many whole event records as fit.
 *
 * `buf` is a kernel buffer the syscall layer copies out afterwards (see the
 * file_operations contract in fs/vfs.h), so this uses memcpy. The fd inside
 * each record has to be minted here rather than when the event was queued,
 * because it must land in the fd table of the process doing the reading.
 */
static s64 fanotify_read(file_t *filp, void *buf, size_t len, u64 *offset)
{
    (void)offset;
    fanotify_context_t *ctx = (fanotify_context_t *)filp->private_data;
    if (!ctx) return -(s64)EINVAL;
    if (len < sizeof(struct fanotify_event_metadata)) return -(s64)EINVAL;

    process_t *proc = sched_current_process();
    u8 *out = (u8 *)buf;
    size_t written = 0;

    irqflags_t fl = spinlock_lock_irqsave(&ctx->lock);

    while (!ctx->event_queue_head) {
        if (filp->f_flags & O_NONBLOCK) {
            spinlock_unlock_irqrestore(&ctx->lock, fl);
            return -(s64)EAGAIN;
        }

        thread_t *curr = sched_current_thread();
        fan_wait_push(&ctx->readers, curr);
        spinlock_unlock_irqrestore(&ctx->lock, fl);

        sched_block(THREAD_BLOCKED_PENDING);

        /* A signal can break the wait; unlink before reporting EINTR so a
         * later wakeup does not touch a thread that has moved on. */
        if (proc && (proc->sig_pending & ~proc->sig_blocked)) {
            fl = spinlock_lock_irqsave(&ctx->lock);
            fan_wait_remove(&ctx->readers, curr);
            spinlock_unlock_irqrestore(&ctx->lock, fl);
            return -(s64)EINTR;
        }
        fl = spinlock_lock_irqsave(&ctx->lock);
    }

    while (written + sizeof(struct fanotify_event_metadata) <= len) {
        fanotify_event_node_t *node = fan_dequeue(ctx);
        if (!node) break;
        spinlock_unlock_irqrestore(&ctx->lock, fl);

        s32 fd = FAN_NOFD;
        if (node->file) {
            s64 got = syscall_install_fd(proc, node->file,
                                         (ctx->flags & FAN_CLOEXEC) ? FD_CLOEXEC : 0);
            if (got >= 0) {
                fd = (s32)got;
            } else {
                /* No descriptor to spare: drop our reference rather than
                 * leaking the open file, and report the event without one. */
                vfs_close(node->file);
            }
            node->file = NULL;
        }
        node->meta.fd = fd;

        memcpy(out + written, &node->meta, sizeof(node->meta));
        written += sizeof(node->meta);
        kfree(node);

        fl = spinlock_lock_irqsave(&ctx->lock);
    }

    spinlock_unlock_irqrestore(&ctx->lock, fl);
    return (s64)written;
}

/*
 * write(2) on a fanotify descriptor delivers a permission decision. No
 * permission event classes are generated here (FAN_OPEN_PERM and friends
 * would have to block the syscall that triggered them), so a well-formed
 * response is accepted and discarded rather than failing a caller that
 * writes one defensively.
 */
static s64 fanotify_write(file_t *filp, const void *buf, size_t len, u64 *offset)
{
    (void)offset;
    fanotify_context_t *ctx = (fanotify_context_t *)filp->private_data;
    if (!ctx) return -(s64)EINVAL;
    if (len < sizeof(struct fanotify_response)) return -(s64)EINVAL;

    struct fanotify_response resp;
    memcpy(&resp, buf, sizeof(resp));
    if (resp.response != FAN_ALLOW && resp.response != FAN_DENY &&
        resp.response != (FAN_ALLOW | FAN_AUDIT) &&
        resp.response != (FAN_DENY | FAN_AUDIT))
        return -(s64)EINVAL;
    return (s64)sizeof(struct fanotify_response);
}

static int fanotify_poll(file_t *filp)
{
    fanotify_context_t *ctx = (fanotify_context_t *)filp->private_data;
    if (!ctx) return 0;

    irqflags_t fl = spinlock_lock_irqsave(&ctx->lock);
    int mask = ctx->event_queue_head ? POLLIN : 0;
    spinlock_unlock_irqrestore(&ctx->lock, fl);
    return mask;
}

/* Unhook @mark from the inode it is attached to. */
static void fan_detach_from_inode(fanotify_mark_t *mark)
{
    if (!mark->inode) return;
    fanotify_mark_t **c = (fanotify_mark_t **)&mark->inode->i_fanotify_marks;
    while (*c) {
        if (*c == mark) { *c = mark->next; mark->next = NULL; return; }
        c = &(*c)->next;
    }
}

static s64 fanotify_release(inode_t *inode, file_t *filp)
{
    (void)inode;
    fanotify_context_t *ctx = (fanotify_context_t *)filp->private_data;
    if (!ctx) return 0;

    irqflags_t fl = spinlock_lock_irqsave(&ctx->lock);

    /* Marks first: once the context is freed, an event on a still-attached
     * mark would queue onto freed memory. */
    fanotify_mark_t *mark = ctx->marks;
    while (mark) {
        fanotify_mark_t *next = mark->ctx_next;
        fan_detach_from_inode(mark);
        kfree(mark);
        mark = next;
    }
    ctx->marks = NULL;

    fanotify_event_node_t *node = ctx->event_queue_head;
    while (node) {
        fanotify_event_node_t *next = node->next;
        if (node->file) vfs_close(node->file);
        kfree(node);
        node = next;
    }
    ctx->event_queue_head = ctx->event_queue_tail = NULL;

    /* Anyone still parked would never be woken again. */
    thread_t *t;
    while ((t = fan_wait_pop(&ctx->readers)) != NULL) {
        spinlock_unlock_irqrestore(&ctx->lock, fl);
        sched_unblock(t);
        fl = spinlock_lock_irqsave(&ctx->lock);
    }

    spinlock_unlock_irqrestore(&ctx->lock, fl);
    kfree(ctx);
    filp->private_data = NULL;
    return 0;
}

static const file_operations_t fanotify_fops = {
    .read    = fanotify_read,
    .write   = fanotify_write,
    .poll    = fanotify_poll,
    .release = fanotify_release,
};

/* ── fanotify_init(2) ────────────────────────────────────────────────────── */

int fanotify_create(unsigned int flags, unsigned int event_f_flags, file_t **out_file)
{
    if (!out_file) return -(s64)EINVAL;

    /* Exactly one notification class, and no unknown bits. */
    unsigned int cls = flags & (FAN_CLASS_CONTENT | FAN_CLASS_PRE_CONTENT);
    if (cls == (FAN_CLASS_CONTENT | FAN_CLASS_PRE_CONTENT)) return -(s64)EINVAL;

    const unsigned int known =
        FAN_CLOEXEC | FAN_NONBLOCK | FAN_CLASS_CONTENT | FAN_CLASS_PRE_CONTENT |
        FAN_UNLIMITED_QUEUE | FAN_UNLIMITED_MARKS | FAN_ENABLE_AUDIT |
        FAN_REPORT_TID | FAN_REPORT_FID | FAN_REPORT_DIR_FID | FAN_REPORT_NAME;
    if (flags & ~known) return -(s64)EINVAL;

    /* The access mode of the descriptors handed out with events. */
    if ((event_f_flags & 3) != O_RDONLY && (event_f_flags & 3) != O_WRONLY &&
        (event_f_flags & 3) != O_RDWR)
        return -(s64)EINVAL;

    fanotify_context_t *ctx = kzalloc(sizeof(fanotify_context_t));
    if (!ctx) return -(s64)ENOMEM;
    spinlock_init(&ctx->lock);
    ctx->flags = flags;
    ctx->event_f_flags = event_f_flags;
    ctx->queue_max = (flags & FAN_UNLIMITED_QUEUE) ? 0 : FAN_DEFAULT_MAX_QUEUE;

    file_t *filp = kzalloc(sizeof(file_t));
    if (!filp) { kfree(ctx); return -(s64)ENOMEM; }
    filp->f_op = (file_operations_t *)&fanotify_fops;
    filp->private_data = ctx;
    filp->f_count = 1;
    if (flags & FAN_NONBLOCK) filp->f_flags |= O_NONBLOCK;

    *out_file = filp;
    return 0;
}

/* ── fanotify_mark(2) ────────────────────────────────────────────────────── */

/* Resolve the (dirfd, pathname) pair fanotify_mark(2) takes into one
 * absolute path, following the same rules as the *at() syscalls: an absolute
 * pathname wins, an empty one names dirfd itself, and AT_FDCWD means the
 * working directory. */
static s64 fan_resolve(int dirfd, const char *pathname, unsigned int flags,
                       char *out, size_t out_len)
{
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)ESRCH;

    if (pathname && pathname[0] == '/') {
        if (vfs_resolve_path("/", pathname, out, out_len) != 0) return -(s64)ENAMETOOLONG;
        return 0;
    }

    char base[256];
    if (dirfd == AT_FDCWD) {
        snprintf(base, sizeof(base), "%s", proc->cwd[0] ? proc->cwd : "/");
    } else {
        file_t *df = syscall_fget(proc, dirfd);
        if (!df) return -(s64)EBADF;
        if (df->f_dentry) dentry_build_path(df->f_dentry, base, sizeof(base));
        else base[0] = '\0';
        syscall_fput(df);
        if (!base[0]) return -(s64)EBADF;
    }

    if (!pathname || !pathname[0]) {
        /* AT_EMPTY_PATH semantics: the mark is on dirfd itself. */
        if (vfs_resolve_path("/", base, out, out_len) != 0) return -(s64)ENAMETOOLONG;
        return 0;
    }

    char joined[512];
    snprintf(joined, sizeof(joined), "%s/%s", base, pathname);
    if (vfs_resolve_path("/", joined, out, out_len) != 0) return -(s64)ENAMETOOLONG;
    (void)flags;
    return 0;
}

int fanotify_add_mark(file_t *filp, unsigned int flags, u64 mask, int dirfd,
                      const char *pathname)
{
    if (!filp || filp->f_op != &fanotify_fops) return -(s64)EBADF;
    fanotify_context_t *ctx = (fanotify_context_t *)filp->private_data;
    if (!ctx) return -(s64)EINVAL;

    unsigned int op = flags & (FAN_MARK_ADD | FAN_MARK_REMOVE | FAN_MARK_FLUSH);
    /* Exactly one operation, and it must be one we know. */
    if (op != FAN_MARK_ADD && op != FAN_MARK_REMOVE && op != FAN_MARK_FLUSH)
        return -(s64)EINVAL;

    if (op == FAN_MARK_FLUSH) {
        irqflags_t fl = spinlock_lock_irqsave(&ctx->lock);
        fanotify_mark_t *m = ctx->marks;
        while (m) {
            fanotify_mark_t *next = m->ctx_next;
            fan_detach_from_inode(m);
            kfree(m);
            m = next;
        }
        ctx->marks = NULL;
        spinlock_unlock_irqrestore(&ctx->lock, fl);
        return 0;
    }

    if (mask == 0) return -(s64)EINVAL;

    char path[256];
    s64 err = fan_resolve(dirfd, pathname, flags, path, sizeof(path));
    if (err < 0) return (int)err;

    dentry_t *dentry = NULL;
    err = (flags & FAN_MARK_DONT_FOLLOW)
              ? vfs_path_lookup_nofollow(path, &dentry)
              : vfs_path_lookup(path, &dentry);
    if (err < 0 || !dentry || !dentry->d_inode) return -(s64)ENOENT;

    inode_t *inode = dentry->d_inode;
    if ((flags & FAN_MARK_ONLYDIR) && !S_ISDIR(inode->i_mode)) return -(s64)ENOTDIR;

    irqflags_t fl = spinlock_lock_irqsave(&ctx->lock);

    /* One mark per (context, inode): a second FAN_MARK_ADD updates the
     * existing one rather than stacking a duplicate, which is what makes
     * repeated adds idempotent the way callers expect. */
    fanotify_mark_t *mark = NULL;
    for (fanotify_mark_t *m = ctx->marks; m; m = m->ctx_next) {
        if (m->inode == inode) { mark = m; break; }
    }

    if (op == FAN_MARK_REMOVE) {
        if (!mark) { spinlock_unlock_irqrestore(&ctx->lock, fl); return -(s64)ENOENT; }
        if (flags & FAN_MARK_IGNORED_MASK) mark->ignored_mask &= ~mask;
        else                               mark->mask &= ~mask;
        /* A mark with nothing left to report is gone. */
        if (mark->mask == 0) {
            fanotify_mark_t **c = &ctx->marks;
            while (*c) {
                if (*c == mark) { *c = mark->ctx_next; break; }
                c = &(*c)->ctx_next;
            }
            fan_detach_from_inode(mark);
            kfree(mark);
        }
        spinlock_unlock_irqrestore(&ctx->lock, fl);
        return 0;
    }

    if (mark) {
        if (flags & FAN_MARK_IGNORED_MASK) mark->ignored_mask |= mask;
        else                               mark->mask |= mask;
        spinlock_unlock_irqrestore(&ctx->lock, fl);
        return 0;
    }

    spinlock_unlock_irqrestore(&ctx->lock, fl);

    mark = kzalloc(sizeof(fanotify_mark_t));
    if (!mark) return -(s64)ENOMEM;
    mark->ctx   = ctx;
    mark->inode = inode;
    if (flags & FAN_MARK_IGNORED_MASK) mark->ignored_mask = mask;
    else                               mark->mask = mask;
    snprintf(mark->path, sizeof(mark->path), "%s", path);

    fl = spinlock_lock_irqsave(&ctx->lock);
    mark->ctx_next = ctx->marks;
    ctx->marks = mark;
    mark->next = (fanotify_mark_t *)inode->i_fanotify_marks;
    inode->i_fanotify_marks = mark;
    spinlock_unlock_irqrestore(&ctx->lock, fl);

    return 0;
}

/* ── event generation ────────────────────────────────────────────────────── */

void vfs_notify_event(inode_t *inode, u64 mask)
{
    if (!inode || !inode->i_fanotify_marks || !mask) return;

    process_t *proc = sched_current_process();
    s32 pid = proc ? (s32)proc->pid : 0;

    for (fanotify_mark_t *mark = (fanotify_mark_t *)inode->i_fanotify_marks;
         mark; mark = mark->next) {
        if (!(mark->mask & mask)) continue;
        if (mark->ignored_mask & mask) continue;

        fanotify_context_t *ctx = mark->ctx;
        if (!ctx) continue;

        /* Open the file the event is about *before* taking the context lock:
         * vfs_open() walks the dcache and must not run with a spinlock held.
         * A context reporting file handles instead of descriptors
         * (FAN_REPORT_FID) does not want one at all. */
        file_t *evfile = NULL;
        if (!(ctx->flags & (FAN_REPORT_FID | FAN_REPORT_DIR_FID)))
            evfile = vfs_open(mark->path, ctx->event_f_flags, 0);

        fanotify_event_node_t *node = kzalloc(sizeof(fanotify_event_node_t));
        if (!node) {
            if (evfile) vfs_close(evfile);
            continue;
        }

        node->meta.event_len    = sizeof(struct fanotify_event_metadata);
        node->meta.vers         = 3;
        node->meta.metadata_len = sizeof(struct fanotify_event_metadata);
        node->meta.mask         = mask;
        node->meta.fd           = FAN_NOFD;
        node->meta.pid          = pid;
        node->file              = evfile;

        irqflags_t fl = spinlock_lock_irqsave(&ctx->lock);

        if (ctx->queue_max && ctx->queue_len >= ctx->queue_max) {
            /* Queue full. Report the loss once — a listener that sees
             * FAN_Q_OVERFLOW knows its view is incomplete — then drop
             * events until it drains. */
            spinlock_unlock_irqrestore(&ctx->lock, fl);
            if (node->file) vfs_close(node->file);
            kfree(node);

            fl = spinlock_lock_irqsave(&ctx->lock);
            if (!ctx->overflowed) {
                fanotify_event_node_t *ov = kzalloc(sizeof(fanotify_event_node_t));
                if (ov) {
                    ov->meta.event_len    = sizeof(struct fanotify_event_metadata);
                    ov->meta.vers         = 3;
                    ov->meta.metadata_len = sizeof(struct fanotify_event_metadata);
                    ov->meta.mask         = FAN_Q_OVERFLOW;
                    ov->meta.fd           = FAN_NOFD;
                    ov->meta.pid          = pid;
                    if (ctx->event_queue_tail) ctx->event_queue_tail->next = ov;
                    else                       ctx->event_queue_head = ov;
                    ctx->event_queue_tail = ov;
                    ctx->queue_len++;
                    ctx->overflowed = true;
                }
            }
            thread_t *w = fan_wait_pop(&ctx->readers);
            spinlock_unlock_irqrestore(&ctx->lock, fl);
            if (w) sched_unblock(w);
            continue;
        }

        if (ctx->event_queue_tail) ctx->event_queue_tail->next = node;
        else                       ctx->event_queue_head = node;
        ctx->event_queue_tail = node;
        ctx->queue_len++;
        ctx->overflowed = false;

        thread_t *waiter = fan_wait_pop(&ctx->readers);
        spinlock_unlock_irqrestore(&ctx->lock, fl);

        if (waiter) sched_unblock(waiter);
    }
}
