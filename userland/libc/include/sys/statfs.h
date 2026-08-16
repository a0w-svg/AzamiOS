/* ============================================================================
 * AzamiOS Userspace — Filesystem Statistics (statfs.h)
 * File: userland/libc/include/sys/statfs.h
 * ============================================================================ */
#pragma once

#include <stdint.h>
#include <stddef.h>

struct statfs {
    uint64_t f_type;
    uint64_t f_bsize;
    uint64_t f_blocks;
    uint64_t f_bfree;
    uint64_t f_bavail;
    uint64_t f_files;
    uint64_t f_ffree;
    uint64_t f_fsid[2];
    uint64_t f_namelen;
    uint64_t f_frsize;
    uint64_t f_flags;
    uint64_t f_spare[4];
};

int statfs(const char *path, struct statfs *buf);
int fstatfs(int fd, struct statfs *buf);
