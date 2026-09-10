/* ============================================================================
 * AzamiOS — POSIX Message Queues (POSIX.1-2008 Message Passing option)
 * File: kernel/ipc/mqueue.h
 *
 *   mq_open / mq_unlink / mq_timedsend / mq_timedreceive /
 *   mq_notify / mq_getsetattr
 *
 * These are a separate mechanism from the System V queues in sysvipc.h, not a
 * second face on the same one, and the differences are visible to programs:
 *
 *   - Queues are named "/name" in a flat namespace, not keyed by ftok().
 *   - A descriptor is a real file descriptor, so poll(), select(), epoll(),
 *     close() and the O_CLOEXEC/O_NONBLOCK flags all work on it.
 *   - Messages carry a *priority* and are received highest-priority-first,
 *     FIFO within a priority — where msgrcv() selects on a caller-chosen type.
 *   - A queue can asynchronously notify one process when it becomes non-empty.
 *   - The name outlives its creator: unlinking is explicit, and an unlinked
 *     queue survives until the last descriptor on it is closed.
 *
 * The ABI (struct mq_attr, the abs_timeout semantics, the errno values) is the
 * Linux x86_64 one, so a libc built for Linux needs no shim.
 * ============================================================================ */
#pragma once

#include "../../include/azami/types.h"
#include "../../fs/vfs.h"

struct process;

/* ── Implementation limits (Linux's compiled-in defaults) ────────────────── */
#define MQ_MAX_QUEUES       64      /* queues that may exist at once        */
#define MQ_NAME_MAX         255     /* excluding the leading '/'            */
#define MQ_PRIO_MAX         32768   /* priorities are 0 .. MQ_PRIO_MAX-1    */
#define MQ_MAXMSG_DEFAULT   10      /* /proc/sys/fs/mqueue/msg_default      */
#define MQ_MSGSIZE_DEFAULT  8192    /* /proc/sys/fs/mqueue/msgsize_default  */
#define MQ_MAXMSG_LIMIT     65536   /* HARD_MSGMAX                          */
#define MQ_MSGSIZE_LIMIT    (16 * 1024 * 1024)
/* Total bytes one queue may hold. Without this a program could ask for
 * 65536 * 16 MiB and take the machine down through a legitimate API. */
#define MQ_QUEUE_BYTES_MAX  (1024 * 1024)

/* ── Linux ABI structures ────────────────────────────────────────────────── */
struct mq_attr {
    s64 mq_flags;      /* 0 or O_NONBLOCK — per *descriptor*, not per queue */
    s64 mq_maxmsg;     /* messages the queue can hold                       */
    s64 mq_msgsize;    /* largest message, in bytes                         */
    s64 mq_curmsgs;    /* messages currently queued                         */
    s64 __reserved[4];
};

/** mqueue_init() — reset the queue table. Called once from kernel_main(). */
void mqueue_init(void);

/**
 * mq_open_impl() — create or open the queue named @uname (a user pointer to
 * "/name"), returning a new file descriptor.
 *
 * @oflag carries O_RDONLY/O_WRONLY/O_RDWR plus O_CREAT, O_EXCL, O_NONBLOCK
 * and O_CLOEXEC. @mode and @uattr are only consulted when O_CREAT actually
 * creates the queue; @uattr may be NULL for the defaults.
 */
s64 mq_open_impl(const char *uname, int oflag, u32 mode, const void *uattr);

/**
 * mq_unlink_impl() — remove the name. Descriptors already open keep working
 * and the queue is freed when the last of them closes, so this is safe to call
 * immediately after mq_open() to get an unnamed queue.
 */
s64 mq_unlink_impl(const char *uname);

/**
 * mq_timedsend_impl() / mq_timedreceive_impl() — the blocking transfer pair.
 *
 * @abs_timeout is an *absolute* CLOCK_REALTIME deadline (POSIX's choice, which
 * makes a retry loop immune to the clock being read between attempts); NULL
 * blocks indefinitely. Both return -ETIMEDOUT when the deadline passes,
 * -EAGAIN when the descriptor is non-blocking, and -EINTR on a signal.
 *
 * Receive always returns the oldest of the highest-priority messages, and the
 * caller's buffer must be at least mq_msgsize bytes — POSIX requires -EMSGSIZE
 * rather than a truncated message.
 */
s64 mq_timedsend_impl(int mqdes, const char *umsg, size_t len, u32 prio,
                      const void *abs_timeout);
s64 mq_timedreceive_impl(int mqdes, char *umsg, size_t len, u32 *uprio,
                         const void *abs_timeout);

/**
 * mq_notify_impl() — register (or, with @usev NULL, deregister) this process
 * for a one-shot signal when the queue goes from empty to non-empty with no
 * reader blocked on it. Only one process at a time may be registered;
 * a second registration gets -EBUSY. The registration is consumed by the
 * notification, so a program that wants a stream of them re-arms each time.
 */
s64 mq_notify_impl(int mqdes, const void *usev);

/**
 * mq_getsetattr_impl() — read and/or replace the descriptor's attributes.
 * Only mq_flags (that is, O_NONBLOCK) can be set; mq_maxmsg and mq_msgsize
 * are fixed when the queue is created. @uoldattr, when non-NULL, receives the
 * state as it was *before* any change.
 */
s64 mq_getsetattr_impl(int mqdes, const void *unewattr, void *uoldattr);

/** mqueue_drop_proc() — release any notification a dying process registered. */
void mqueue_drop_proc(u32 pid);

/** Format /proc/mqueues-style accounting into @buf. */
size_t mqueue_format_proc(char *buf, size_t max);
