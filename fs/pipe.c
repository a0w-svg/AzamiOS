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
        if (filp->f_inode) kfree(filp->f_inode);
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
        if (filp->f_inode) kfree(filp->f_inode);
        kfree(pipe);
    }
    return 0;
}

static file_operations_t pipe_read_fops = {
    .read = pipe_read,
    .write = NULL,
    .release = pipe_read_release,
};

static file_operations_t pipe_write_fops = {
    .read = NULL,
    .write = pipe_write,
    .release = pipe_write_release,
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
