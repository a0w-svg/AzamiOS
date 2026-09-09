/* ============================================================================
 * AzamiOS — UNIX Pipe (FIFO) Implementation Header
 * File: fs/pipe.h
 * ============================================================================ */
#pragma once

#include "vfs.h"
#include "../arch/x86_64/cpu/spinlock.h"
#include "../kernel/sched/sched.h"

#define PIPE_BUFFER_SIZE 4096

typedef struct pipe {
    spinlock_t  lock;
    u8          buffer[PIPE_BUFFER_SIZE];
    u32         read_pos;
    u32         write_pos;
    u32         count;
    u32         readers;
    u32         writers;
    thread_t   *read_wait;
    thread_t   *write_wait;
    inode_t    *inode;
} pipe_t;

/** pipe_create() — Allocate a pipe and return read and write file descriptions. */
int pipe_create(file_t **read_file, file_t **write_file);

/** sockpair_create() — Allocate a bidirectional socket pair (AF_UNIX socketpair). */
int sockpair_create(file_t **file1, file_t **file2);

/** pipe_read_pipe()/pipe_write_pipe() — read/write a `pipe_t` directly, given
 *  only the pipe itself rather than one of the two file_t wrappers
 *  pipe_create() hands back. Same trick sockpair_read()/sockpair_write() in
 *  pipe.c already use internally (a throwaway on-stack file_t just to carry
 *  f_flags/private_data into the real pipe_read()/pipe_write()), exported so
 *  kernel/net/socket.c's AF_UNIX SOCK_STREAM backend — which needs a
 *  g_socket_fops-backed fd, not a sockpair_fops one, so every other socket
 *  syscall (getsockname, sendmsg/recvmsg, ...) still recognizes the fd as a
 *  socket — can reuse pipe.c's byte-stream buffering without duplicating it. */
s64 pipe_read_pipe(pipe_t *p, void *buf, size_t len, bool nonblock);
s64 pipe_write_pipe(pipe_t *p, const void *buf, size_t len, bool nonblock);

/** pipe_close_end() — release one end of a pipe_t obtained via the raw
 *  tx_pipe/rx_pipe pointers above (decrements readers or writers and frees
 *  the pipe once both hit zero), for a caller that never went through
 *  pipe_create()'s own file_t/release path. `is_reader` selects which count
 *  to decrement — the same pipe_t is one end's tx_pipe and the other end's
 *  rx_pipe, so which side *this* caller was using isn't recoverable from the
 *  pipe_t alone. */
void pipe_close_end(pipe_t *p, bool is_reader);
