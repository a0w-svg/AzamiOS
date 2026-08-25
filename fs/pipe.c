/* ============================================================================
 * AzamiOS — UNIX Pipe (FIFO) Implementation
 * File: fs/pipe.c
 * ============================================================================ */

#include "pipe.h"
#include "../kernel/mm/kmalloc.h"
#include "../kernel/lib/string.h"
#include "../include/azami/defs.h"

static void pipe_wait_push(thread_t **queue, thread_t *t)
{
    t->next = *queue;
    *queue = t;
}

static thread_t *pipe_wait_pop(thread_t **queue)
{
    if (!*queue) return NULL;
    thread_t *t = *queue;
    *queue = t->next;
    t->next = NULL;
    return t;
}

static s64 pipe_read(file_t *filp, void *buf, size_t len, u64 *offset)
{
    (void)offset;
    pipe_t *pipe = (pipe_t *)filp->private_data;
    if (!pipe || !buf) return -(s64)EINVAL;
    if (len == 0) return 0;

    bool nonblock = !!(filp->f_flags & O_NONBLOCK);
    u8 *dst = (u8 *)buf;
    size_t bytes_read = 0;

    for (;;) {
        spinlock_lock(&pipe->lock);

        if (pipe->count > 0) {
            while (bytes_read < len && pipe->count > 0) {
                dst[bytes_read++] = pipe->buffer[pipe->read_pos];
                pipe->read_pos = (pipe->read_pos + 1) % PIPE_BUFFER_SIZE;
                pipe->count--;
            }

            thread_t *writer = pipe_wait_pop(&pipe->write_wait);
            spinlock_unlock(&pipe->lock);

            if (writer) {
                sched_unblock(writer);
            }
            return (s64)bytes_read;
        }

        /* Buffer is empty */
        if (pipe->writers == 0) {
            spinlock_unlock(&pipe->lock);
            return 0; /* EOF */
        }

        /* POSIX-08: return EAGAIN immediately if O_NONBLOCK is set */
        if (nonblock) {
            spinlock_unlock(&pipe->lock);
            return (bytes_read > 0) ? (s64)bytes_read : -(s64)EAGAIN;
        }

        /* Block receiver */
        thread_t *curr = sched_current_thread();
        pipe_wait_push(&pipe->read_wait, curr);
        spinlock_unlock(&pipe->lock);

        sched_block(THREAD_BLOCKED);
    }
}

static s64 pipe_write(file_t *filp, const void *buf, size_t len, u64 *offset)
{
    (void)offset;
    pipe_t *pipe = (pipe_t *)filp->private_data;
    if (!pipe || !buf) return -(s64)EINVAL;
    if (len == 0) return 0;

    bool nonblock = !!(filp->f_flags & O_NONBLOCK);
    const u8 *src = (const u8 *)buf;
    size_t bytes_written = 0;

    for (;;) {
        spinlock_lock(&pipe->lock);

        if (pipe->readers == 0) {
            spinlock_unlock(&pipe->lock);
            return -(s64)EPIPE;
        }

        if (pipe->count < PIPE_BUFFER_SIZE) {
            while (bytes_written < len && pipe->count < PIPE_BUFFER_SIZE) {
                pipe->buffer[pipe->write_pos] = src[bytes_written++];
                pipe->write_pos = (pipe->write_pos + 1) % PIPE_BUFFER_SIZE;
                pipe->count++;
            }

            thread_t *reader = pipe_wait_pop(&pipe->read_wait);
            spinlock_unlock(&pipe->lock);

            if (reader) {
                sched_unblock(reader);
            }
            if (bytes_written == len) {
                return (s64)bytes_written;
            }
            /* Partial write — loop to write the rest */
            if (nonblock) return (s64)bytes_written;
            continue;
        }

        /* Buffer is full */
        /* POSIX-08: return EAGAIN immediately if O_NONBLOCK is set */
        if (nonblock) {
            spinlock_unlock(&pipe->lock);
            return (bytes_written > 0) ? (s64)bytes_written : -(s64)EAGAIN;
        }

        /* Block sender */
        thread_t *curr = sched_current_thread();
        pipe_wait_push(&pipe->write_wait, curr);
        spinlock_unlock(&pipe->lock);

        sched_block(THREAD_BLOCKED);
    }
}

static s64 pipe_read_release(struct inode *inode, file_t *filp)
{
    (void)inode;
    pipe_t *pipe = (pipe_t *)filp->private_data;
    if (!pipe) return 0;

    spinlock_lock(&pipe->lock);
    if (pipe->readers > 0) pipe->readers--;

    /* Wake up any writers blocked on full pipe */
    while (pipe->write_wait) {
        thread_t *w = pipe_wait_pop(&pipe->write_wait);
        if (w) sched_unblock(w);
    }

    bool should_free = (pipe->readers == 0 && pipe->writers == 0);
    spinlock_unlock(&pipe->lock);

    if (should_free) {
        if (pipe->inode) {
            kfree(pipe->inode);
            pipe->inode = NULL;
        }
        kfree(pipe);
    }
    return 0;
}

static s64 pipe_write_release(struct inode *inode, file_t *filp)
{
    (void)inode;
    pipe_t *pipe = (pipe_t *)filp->private_data;
    if (!pipe) return 0;

    spinlock_lock(&pipe->lock);
    if (pipe->writers > 0) pipe->writers--;

    /* Wake up any readers blocked on empty pipe so they see EOF */
    while (pipe->read_wait) {
        thread_t *r = pipe_wait_pop(&pipe->read_wait);
        if (r) sched_unblock(r);
    }

    bool should_free = (pipe->readers == 0 && pipe->writers == 0);
    spinlock_unlock(&pipe->lock);

    if (should_free) {
        if (pipe->inode) {
            kfree(pipe->inode);
            pipe->inode = NULL;
        }
        kfree(pipe);
    }
    return 0;
}

#include "../kernel/uaccess.h"

#ifndef POLLIN
#define POLLIN      0x0001
#define POLLPRI     0x0002
#define POLLOUT     0x0004
#define POLLERR     0x0008
#define POLLHUP     0x0010
#define POLLNVAL    0x0020
#define POLLRDNORM  0x0040
#define POLLWRNORM  0x0100
#endif

static int pipe_read_poll(file_t *filp)
{
    if (!filp || !filp->private_data) return POLLNVAL;
    pipe_t *pipe = (pipe_t *)filp->private_data;

    int mask = 0;
    spinlock_lock(&pipe->lock);
    if (pipe->count > 0) {
        mask |= (POLLIN | POLLRDNORM);
    }
    if (pipe->writers == 0) {
        mask |= POLLHUP;
    }
    spinlock_unlock(&pipe->lock);
    return mask;
}

static int pipe_write_poll(file_t *filp)
{
    if (!filp || !filp->private_data) return POLLNVAL;
    pipe_t *pipe = (pipe_t *)filp->private_data;

    int mask = 0;
    spinlock_lock(&pipe->lock);
    if (pipe->readers == 0) {
        mask |= (POLLERR | POLLHUP);
    } else if (pipe->count < PIPE_BUFFER_SIZE) {
        mask |= (POLLOUT | POLLWRNORM);
    }
    spinlock_unlock(&pipe->lock);
    return mask;
}

static s64 pipe_ioctl(file_t *filp, u32 cmd, u64 arg)
{
    if (!filp || !filp->private_data) return -(s64)EBADF;
    pipe_t *pipe = (pipe_t *)filp->private_data;

    if (cmd == 0x541B /* FIONREAD */) {
        if (!arg || (uintptr_t)arg >= 0x8000000000000000ULL) return -(s64)EINVAL;
        int nbytes = (int)pipe->count;
        if (copy_to_user((void *)(uintptr_t)arg, &nbytes, sizeof(int)) != 0) return -(s64)EFAULT;
        return 0;
    }

    if (cmd == 0x5421 /* FIONBIO */) {
        if (!arg || (uintptr_t)arg >= 0x8000000000000000ULL) return -(s64)EINVAL;
        int val = 0;
        if (copy_from_user(&val, (const void *)(uintptr_t)arg, sizeof(int)) != 0) return -(s64)EFAULT;
        if (val) filp->f_flags |= O_NONBLOCK;
        else filp->f_flags &= ~O_NONBLOCK;
        return 0;
    }

    return -(s64)EINVAL;
}

static file_operations_t pipe_read_fops = {
    .read = pipe_read,
    .write = NULL,
    .ioctl = pipe_ioctl,
    .release = pipe_read_release,
    .poll = pipe_read_poll,
};

static file_operations_t pipe_write_fops = {
    .read = NULL,
    .write = pipe_write,
    .ioctl = pipe_ioctl,
    .release = pipe_write_release,
    .poll = pipe_write_poll,
};

int pipe_create(file_t **read_file, file_t **write_file)
{
    if (!read_file || !write_file) return -(s64)EINVAL;

    pipe_t *pipe = (pipe_t *)kzalloc(sizeof(pipe_t));
    if (!pipe) return -(s64)ENOMEM;

    pipe->lock = (spinlock_t)SPINLOCK_INIT;
    pipe->read_pos = 0;
    pipe->write_pos = 0;
    pipe->count = 0;
    pipe->readers = 1;
    pipe->writers = 1;
    pipe->read_wait = NULL;
    pipe->write_wait = NULL;

    inode_t *pinode = (inode_t *)kzalloc(sizeof(inode_t));
    if (!pinode) {
        kfree(pipe);
        return -(s64)ENOMEM;
    }
    pinode->i_mode = S_IFIFO | 0600;
    pinode->i_size = 0;
    pinode->i_blocks = 0;
    pinode->i_ino = (u64)(uintptr_t)pipe;

    pipe->inode = pinode;

    file_t *rf = (file_t *)kzalloc(sizeof(file_t));
    if (!rf) {
        kfree(pinode);
        kfree(pipe);
        return -(s64)ENOMEM;
    }
    file_t *wf = (file_t *)kzalloc(sizeof(file_t));
    if (!wf) {
        kfree(rf);
        kfree(pinode);
        kfree(pipe);
        return -(s64)ENOMEM;
    }

    rf->f_inode = pinode;
    rf->f_op = &pipe_read_fops;
    rf->f_flags = O_RDONLY;
    rf->f_count = 1;
    rf->private_data = pipe;

    wf->f_inode = pinode;
    wf->f_op = &pipe_write_fops;
    wf->f_flags = O_WRONLY;
    wf->f_count = 1;
    wf->private_data = pipe;

    *read_file = rf;
    *write_file = wf;
    return 0;
}

typedef struct {
    pipe_t  *tx_pipe;
    pipe_t  *rx_pipe;
    inode_t *inode;
} sockpair_ep_t;

static s64 sockpair_read(file_t *filp, void *buf, size_t len, u64 *offset)
{
    sockpair_ep_t *ep = (sockpair_ep_t *)filp->private_data;
    if (!ep || !ep->rx_pipe) return -(s64)EINVAL;
    file_t tmp_filp;
    memset(&tmp_filp, 0, sizeof(tmp_filp));
    tmp_filp.f_flags = filp->f_flags;
    tmp_filp.private_data = ep->rx_pipe;
    return pipe_read(&tmp_filp, buf, len, offset);
}

static s64 sockpair_write(file_t *filp, const void *buf, size_t len, u64 *offset)
{
    sockpair_ep_t *ep = (sockpair_ep_t *)filp->private_data;
    if (!ep || !ep->tx_pipe) return -(s64)EINVAL;
    file_t tmp_filp;
    memset(&tmp_filp, 0, sizeof(tmp_filp));
    tmp_filp.f_flags = filp->f_flags;
    tmp_filp.private_data = ep->tx_pipe;
    return pipe_write(&tmp_filp, buf, len, offset);
}

static s64 sockpair_release(struct inode *inode, file_t *filp)
{
    (void)inode;
    sockpair_ep_t *ep = (sockpair_ep_t *)filp->private_data;
    if (!ep) return 0;

    if (ep->tx_pipe) {
        file_t tmp_tx;
        memset(&tmp_tx, 0, sizeof(tmp_tx));
        tmp_tx.private_data = ep->tx_pipe;
        pipe_write_release(NULL, &tmp_tx);
    }
    if (ep->rx_pipe) {
        file_t tmp_rx;
        memset(&tmp_rx, 0, sizeof(tmp_rx));
        tmp_rx.private_data = ep->rx_pipe;
        pipe_read_release(NULL, &tmp_rx);
    }
    kfree(ep);
    return 0;
}

static file_operations_t sockpair_fops = {
    .read = sockpair_read,
    .write = sockpair_write,
    .release = sockpair_release,
};

int sockpair_create(file_t **file1, file_t **file2)
{
    if (!file1 || !file2) return -(s64)EINVAL;

    file_t *r1 = NULL, *w1 = NULL;
    file_t *r2 = NULL, *w2 = NULL;
    int err1 = pipe_create(&r1, &w1);
    if (err1 < 0) return err1;
    int err2 = pipe_create(&r2, &w2);
    if (err2 < 0) {
        vfs_close(r1);
        vfs_close(w1);
        return err2;
    }

    sockpair_ep_t *ep1 = (sockpair_ep_t *)kzalloc(sizeof(sockpair_ep_t));
    sockpair_ep_t *ep2 = (sockpair_ep_t *)kzalloc(sizeof(sockpair_ep_t));
    if (!ep1 || !ep2) {
        if (ep1) kfree(ep1);
        if (ep2) kfree(ep2);
        vfs_close(r1); vfs_close(w1);
        vfs_close(r2); vfs_close(w2);
        return -(s64)ENOMEM;
    }

    ep1->tx_pipe = (pipe_t *)w1->private_data;
    ep1->rx_pipe = (pipe_t *)r2->private_data;
    ep1->inode   = w1->f_inode;

    ep2->tx_pipe = (pipe_t *)w2->private_data;
    ep2->rx_pipe = (pipe_t *)r1->private_data;
    ep2->inode   = w2->f_inode;

    file_t *f1 = (file_t *)kzalloc(sizeof(file_t));
    file_t *f2 = (file_t *)kzalloc(sizeof(file_t));
    if (!f1 || !f2) {
        if (f1) kfree(f1);
        if (f2) kfree(f2);
        kfree(ep1); kfree(ep2);
        vfs_close(r1); vfs_close(w1);
        vfs_close(r2); vfs_close(w2);
        return -(s64)ENOMEM;
    }

    f1->f_inode = ep1->inode;
    f1->f_op = &sockpair_fops;
    f1->f_flags = O_RDWR;
    f1->f_count = 1;
    f1->private_data = ep1;

    f2->f_inode = ep2->inode;
    f2->f_op = &sockpair_fops;
    f2->f_flags = O_RDWR;
    f2->f_count = 1;
    f2->private_data = ep2;

    /* Free unused wrapping file headers from pipe_create */
    kfree(r1); kfree(w1);
    kfree(r2); kfree(w2);

    *file1 = f1;
    *file2 = f2;
    return 0;
}
