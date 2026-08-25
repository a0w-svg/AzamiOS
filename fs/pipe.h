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
