/* ============================================================================
 * AzamiOS — Linux asynchronous I/O (io_setup/io_submit/io_getevents/...)
 * File: fs/aio.c
 *
 * include/azami/uapi/aio_abi.h has described this ABI since it was written,
 * but none of the five syscalls existed, so every program built against
 * libaio — or against glibc's POSIX aio_*(3), which uses it when present —
 * got -ENOSYS from io_setup(2) and either fell back to synchronous I/O or
 * refused to start. Databases and anything using io_uring's older sibling
 * are the usual callers.
 *
 * Submissions are carried out synchronously inside io_submit(2) and their
 * results queued as completions. That is a legal implementation of the
 * interface — io_submit(2) is permitted to block, and nothing in the
 * contract promises an operation is still outstanding when it returns — and
 * it is the honest one here, since there is no I/O thread pool to hand work
 * to. What userspace observes is a correctly behaving AIO context: every
 * submitted iocb produces exactly one io_event carrying its result, in
 * submission order, retrievable with io_getevents(2).
 * ============================================================================ */

#include "vfs.h"
#include "aio.h"
#include "../kernel/mm/kmalloc.h"
#include "../kernel/lib/string.h"
#include "../arch/x86_64/cpu/spinlock.h"
#include "../kernel/sched/sched.h"
#include "../kernel/syscall/syscall.h"

extern int copy_to_user(void *dst, const void *src, size_t size);
extern int copy_from_user(void *dst, const void *src, size_t size);

/* A context's ring is sized by the caller. Cap it so a single io_setup()
 * cannot ask the kernel for an unbounded allocation. */
#define AIO_MAX_EVENTS      4096
/* Total contexts across the system — the "aio-max-nr" sysctl's role. */
#define AIO_MAX_CONTEXTS    64
/* Staging-buffer chunk for one transfer, matching what pread64/pwrite64
 * already use for the same reason. */
#define AIO_CHUNK           65536

typedef struct aio_ctx {
    u64            id;          /* the aio_context_t handed to userspace */
    process_t     *owner;       /* only the creating process may use it */
    u32            nr_events;   /* ring capacity */
    struct io_event *ring;
    u32            head;        /* next event to hand out */
    u32            count;       /* events waiting */
    spinlock_t     lock;
    bool           in_use;
} aio_ctx_t;

static aio_ctx_t  g_aio_ctxs[AIO_MAX_CONTEXTS];
static spinlock_t g_aio_lock = SPINLOCK_INIT;
static u64        g_aio_next_id = 1;

/* Look up a context by id and check the caller owns it. */
static aio_ctx_t *aio_lookup(u64 id, process_t *proc)
{
    if (!id) return NULL;
    spinlock_lock(&g_aio_lock);
    aio_ctx_t *found = NULL;
    for (int i = 0; i < AIO_MAX_CONTEXTS; i++) {
        if (g_aio_ctxs[i].in_use && g_aio_ctxs[i].id == id) {
            /* A context belongs to the process that created it. Without
             * this any process could guess an id and drain another's
             * completions. */
            if (g_aio_ctxs[i].owner == proc) found = &g_aio_ctxs[i];
            break;
        }
    }
    spinlock_unlock(&g_aio_lock);
    return found;
}

s64 aio_setup(u32 nr_events, u64 *out_id)
{
    if (nr_events == 0 || nr_events > AIO_MAX_EVENTS) return -(s64)EINVAL;
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    /* Linux over-allocates the ring relative to the requested nr_events;
     * one slot per requested event is the minimum that satisfies the
     * contract, which is what this does. */
    struct io_event *ring = (struct io_event *)kzalloc(sizeof(struct io_event) * nr_events);
    if (!ring) return -(s64)ENOMEM;

    spinlock_lock(&g_aio_lock);
    aio_ctx_t *ctx = NULL;
    for (int i = 0; i < AIO_MAX_CONTEXTS; i++) {
        if (!g_aio_ctxs[i].in_use) { ctx = &g_aio_ctxs[i]; break; }
    }
    if (!ctx) {
        spinlock_unlock(&g_aio_lock);
        kfree(ring);
        return -(s64)EAGAIN;    /* aio-max-nr exhausted */
    }
    ctx->id        = g_aio_next_id++;
    ctx->owner     = proc;
    ctx->nr_events = nr_events;
    ctx->ring      = ring;
    ctx->head      = 0;
    ctx->count     = 0;
    ctx->in_use    = true;
    spinlock_init(&ctx->lock);
    u64 id = ctx->id;
    spinlock_unlock(&g_aio_lock);

    *out_id = id;
    return 0;
}

s64 aio_destroy(u64 id)
{
    process_t *proc = sched_current_process();
    aio_ctx_t *ctx = aio_lookup(id, proc);
    if (!ctx) return -(s64)EINVAL;

    spinlock_lock(&g_aio_lock);
    struct io_event *ring = ctx->ring;
    ctx->ring   = NULL;
    ctx->in_use = false;
    ctx->id     = 0;
    ctx->owner  = NULL;
    ctx->count  = 0;
    spinlock_unlock(&g_aio_lock);

    kfree(ring);
    return 0;
}

void aio_process_exit(process_t *proc)
{
    if (!proc) return;
    spinlock_lock(&g_aio_lock);
    for (int i = 0; i < AIO_MAX_CONTEXTS; i++) {
        if (!g_aio_ctxs[i].in_use || g_aio_ctxs[i].owner != proc) continue;
        struct io_event *ring = g_aio_ctxs[i].ring;
        g_aio_ctxs[i].in_use = false;
        g_aio_ctxs[i].id     = 0;
        g_aio_ctxs[i].owner  = NULL;
        g_aio_ctxs[i].ring   = NULL;
        g_aio_ctxs[i].count  = 0;
        spinlock_unlock(&g_aio_lock);
        kfree(ring);
        spinlock_lock(&g_aio_lock);
    }
    spinlock_unlock(&g_aio_lock);
}

/* Append one completion. Returns false if the ring is full, which is the
 * caller's signal to stop submitting (io_submit reports EAGAIN). */
static bool aio_complete(aio_ctx_t *ctx, u64 data, u64 obj, s64 res, s64 res2)
{
    irqflags_t fl = spinlock_lock_irqsave(&ctx->lock);
    if (ctx->count >= ctx->nr_events) {
        spinlock_unlock_irqrestore(&ctx->lock, fl);
        return false;
    }
    u32 slot = (ctx->head + ctx->count) % ctx->nr_events;
    ctx->ring[slot].data = data;
    ctx->ring[slot].obj  = obj;
    ctx->ring[slot].res  = res;
    ctx->ring[slot].res2 = res2;
    ctx->count++;
    spinlock_unlock_irqrestore(&ctx->lock, fl);
    return true;
}

/* An iocb that asked for eventfd notification (IOCB_FLAG_RESFD) gets a
 * counter bump on that descriptor when it completes, so a poll loop on the
 * eventfd learns there is something to collect. */
#define IOCB_FLAG_RESFD  (1u << 0)

static void aio_signal_eventfd(process_t *proc, u32 resfd)
{
    file_t *ef = syscall_fget(proc, (int)resfd);
    if (!ef) return;
    if (ef->f_op && ef->f_op->write) {
        u64 one = 1;
        u64 pos = 0;
        /* The fop write contract takes a kernel pointer, which is what
         * `one` is — see the note in fs/vfs.h. */
        (void)ef->f_op->write(ef, &one, sizeof(one), &pos);
    }
    syscall_fput(ef);
}

/* Carry out one iocb. Returns the value that goes in io_event.res: a byte
 * count, or a negative errno. */
static s64 aio_run_one(process_t *proc, const struct iocb *cb)
{
    file_t *file = syscall_fget(proc, (int)cb->aio_fildes);
    if (!file) return -(s64)EBADF;

    s64 result;
    switch (cb->aio_lio_opcode) {
    case IOCB_CMD_NOOP:
        result = 0;
        break;

    case IOCB_CMD_FSYNC:
    case IOCB_CMD_FDSYNC:
        result = file->f_inode ? vfs_sync_fs(file->f_inode->i_sb) : 0;
        break;

    case IOCB_CMD_PREAD: {
        void *ubuf   = (void *)(uintptr_t)cb->aio_buf;
        size_t count = (size_t)cb->aio_nbytes;
        if (cb->aio_offset < 0) { result = -(s64)EINVAL; break; }
        if (count == 0) { result = 0; break; }
        if ((uintptr_t)ubuf >= 0x0000800000000000ULL) { result = -(s64)EFAULT; break; }

        size_t cap = count > AIO_CHUNK ? AIO_CHUNK : count;
        char *kbuf = (char *)kmalloc(cap);
        if (!kbuf) { result = -(s64)ENOMEM; break; }

        size_t done = 0;
        s64 err = 0;
        while (done < count) {
            size_t chunk = count - done;
            if (chunk > cap) chunk = cap;
            u64 saved = file->f_pos;
            file->f_pos = (u64)cb->aio_offset + done;
            s64 n = vfs_read(file, kbuf, chunk);
            file->f_pos = saved;
            if (n < 0) { err = n; break; }
            if (n == 0) break;                       /* EOF: short read */
            if (copy_to_user((char *)ubuf + done, kbuf, (size_t)n) != 0) {
                err = -(s64)EFAULT;
                break;
            }
            done += (size_t)n;
            if ((size_t)n < chunk) break;
        }
        kfree(kbuf);
        result = (done > 0) ? (s64)done : err;
        break;
    }

    case IOCB_CMD_PWRITE: {
        const void *ubuf = (const void *)(uintptr_t)cb->aio_buf;
        size_t count = (size_t)cb->aio_nbytes;
        if (cb->aio_offset < 0) { result = -(s64)EINVAL; break; }
        if (count == 0) { result = 0; break; }
        if ((uintptr_t)ubuf >= 0x0000800000000000ULL) { result = -(s64)EFAULT; break; }

        size_t cap = count > AIO_CHUNK ? AIO_CHUNK : count;
        char *kbuf = (char *)kmalloc(cap);
        if (!kbuf) { result = -(s64)ENOMEM; break; }

        size_t done = 0;
        s64 err = 0;
        while (done < count) {
            size_t chunk = count - done;
            if (chunk > cap) chunk = cap;
            if (copy_from_user(kbuf, (const char *)ubuf + done, chunk) != 0) {
                err = -(s64)EFAULT;
                break;
            }
            u64 saved = file->f_pos;
            file->f_pos = (u64)cb->aio_offset + done;
            s64 n = vfs_write(file, kbuf, chunk);
            file->f_pos = saved;
            if (n < 0) { err = n; break; }
            if (n == 0) break;
            done += (size_t)n;
            if ((size_t)n < chunk) break;
        }
        kfree(kbuf);
        result = (done > 0) ? (s64)done : err;
        break;
    }

    case IOCB_CMD_PREADV:
    case IOCB_CMD_PWRITEV: {
        /* aio_buf points at an iovec array, aio_nbytes is its length. Each
         * segment is issued at the running offset, exactly as the scatter/
         * gather form requires. */
        struct { void *base; size_t len; } iov;
        u64 nsegs = cb->aio_nbytes;
        if (nsegs > 1024) { result = -(s64)EINVAL; break; }
        const u8 *uiov = (const u8 *)(uintptr_t)cb->aio_buf;
        if ((uintptr_t)uiov >= 0x0000800000000000ULL) { result = -(s64)EFAULT; break; }

        s64 total = 0;
        s64 err = 0;
        u64 off = (u64)cb->aio_offset;
        for (u64 i = 0; i < nsegs; i++) {
            if (copy_from_user(&iov, uiov + i * sizeof(iov), sizeof(iov)) != 0) {
                err = -(s64)EFAULT;
                break;
            }
            if (iov.len == 0) continue;
            struct iocb sub = *cb;
            sub.aio_lio_opcode = (cb->aio_lio_opcode == IOCB_CMD_PREADV)
                                     ? IOCB_CMD_PREAD : IOCB_CMD_PWRITE;
            sub.aio_buf    = (u64)(uintptr_t)iov.base;
            sub.aio_nbytes = iov.len;
            sub.aio_offset = (s64)off;
            s64 n = aio_run_one(proc, &sub);
            if (n < 0) { err = n; break; }
            total += n;
            off   += (u64)n;
            if ((size_t)n < iov.len) break;          /* short: stop here */
        }
        result = (total > 0) ? total : err;
        break;
    }

    default:
        result = -(s64)EINVAL;
        break;
    }

    syscall_fput(file);
    return result;
}

s64 aio_submit(u64 id, long nr, u64 *user_iocb_ptrs)
{
    if (nr < 0) return -(s64)EINVAL;
    if (nr == 0) return 0;

    process_t *proc = sched_current_process();
    aio_ctx_t *ctx = aio_lookup(id, proc);
    if (!ctx) return -(s64)EINVAL;

    long submitted = 0;
    for (long i = 0; i < nr; i++) {
        u64 ucb = 0;
        if (copy_from_user(&ucb, user_iocb_ptrs + i, sizeof(ucb)) != 0)
            return submitted ? submitted : -(s64)EFAULT;
        if (!ucb || ucb >= 0x0000800000000000ULL)
            return submitted ? submitted : -(s64)EFAULT;

        struct iocb cb;
        if (copy_from_user(&cb, (const void *)(uintptr_t)ucb, sizeof(cb)) != 0)
            return submitted ? submitted : -(s64)EFAULT;

        s64 res = aio_run_one(proc, &cb);

        /* The ring being full is not an error for the iocbs already
         * accepted — io_submit(2) returns how many it took. */
        if (!aio_complete(ctx, cb.aio_data, ucb, res, 0))
            return submitted ? submitted : -(s64)EAGAIN;

        if (cb.aio_flags & IOCB_FLAG_RESFD) aio_signal_eventfd(proc, cb.aio_resfd);
        submitted++;
    }
    return submitted;
}

s64 aio_getevents(u64 id, long min_nr, long nr, struct io_event *user_events,
                  u64 timeout_ns, bool have_timeout)
{
    if (nr < 0 || min_nr < 0 || min_nr > nr) return -(s64)EINVAL;
    if (nr == 0) return 0;

    process_t *proc = sched_current_process();
    aio_ctx_t *ctx = aio_lookup(id, proc);
    if (!ctx) return -(s64)EINVAL;

    /* Submissions complete inside io_submit(), so there is normally
     * something waiting the moment this is called. A caller that asks for
     * more than were submitted still gets the blocking behaviour it
     * expects, bounded by its own timeout. */
    u64 waited_ns = 0;
    const u64 tick_ns = 10000000ull;    /* one 100 Hz scheduler tick */
    for (;;) {
        irqflags_t fl = spinlock_lock_irqsave(&ctx->lock);
        u32 avail = ctx->count;
        spinlock_unlock_irqrestore(&ctx->lock, fl);

        if (avail >= (u32)min_nr || min_nr == 0) break;
        if (have_timeout && waited_ns >= timeout_ns) break;
        if (proc && (proc->sig_pending & ~proc->sig_blocked)) return -(s64)EINTR;

        sched_sleep(1);
        waited_ns += tick_ns;
    }

    long copied = 0;
    while (copied < nr) {
        struct io_event ev;
        irqflags_t fl = spinlock_lock_irqsave(&ctx->lock);
        if (ctx->count == 0) { spinlock_unlock_irqrestore(&ctx->lock, fl); break; }
        ev = ctx->ring[ctx->head];
        ctx->head = (ctx->head + 1) % ctx->nr_events;
        ctx->count--;
        spinlock_unlock_irqrestore(&ctx->lock, fl);

        if (copy_to_user(&user_events[copied], &ev, sizeof(ev)) != 0)
            return copied ? copied : -(s64)EFAULT;
        copied++;
    }
    return copied;
}

s64 aio_cancel(u64 id, u64 user_iocb, struct io_event *user_result)
{
    process_t *proc = sched_current_process();
    aio_ctx_t *ctx = aio_lookup(id, proc);
    if (!ctx) return -(s64)EINVAL;

    /* Nothing is ever outstanding: io_submit() runs each iocb to completion
     * before it returns. If the completion is still sitting in the ring,
     * hand it back and remove it — that is what a successful cancel looks
     * like to the caller. Otherwise there is nothing to cancel. */
    irqflags_t fl = spinlock_lock_irqsave(&ctx->lock);
    for (u32 i = 0; i < ctx->count; i++) {
        u32 slot = (ctx->head + i) % ctx->nr_events;
        if (ctx->ring[slot].obj != user_iocb) continue;

        struct io_event ev = ctx->ring[slot];
        /* Close the gap so the queue stays contiguous. */
        for (u32 j = i; j + 1 < ctx->count; j++) {
            u32 a = (ctx->head + j) % ctx->nr_events;
            u32 b = (ctx->head + j + 1) % ctx->nr_events;
            ctx->ring[a] = ctx->ring[b];
        }
        ctx->count--;
        spinlock_unlock_irqrestore(&ctx->lock, fl);

        if (user_result && copy_to_user(user_result, &ev, sizeof(ev)) != 0)
            return -(s64)EFAULT;
        return 0;
    }
    spinlock_unlock_irqrestore(&ctx->lock, fl);
    return -(s64)EINVAL;   /* already reaped, or never submitted here */
}
