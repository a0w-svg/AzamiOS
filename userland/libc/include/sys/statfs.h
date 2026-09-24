/* ============================================================================
 * AzamiOS Userspace — Filesystem Statistics (statfs.h)
 * File: userland/libc/include/sys/statfs.h
 * ============================================================================ */
#pragma once

#include <stdint.h>
#include <stddef.h>

/* Matches the Linux x86_64 struct statfs field for field — which is what the
 * kernel fills in. f_fsid is fsid_t: two 32-bit words, 8 bytes total. It was
 * declared uint64_t[2] here, so f_namelen, f_frsize and f_flags were all read
 * 8 bytes past where the kernel wrote them. */
struct statfs {
    uint64_t f_type;
    uint64_t f_bsize;
    uint64_t f_blocks;
    uint64_t f_bfree;
    uint64_t f_bavail;
    uint64_t f_files;
    uint64_t f_ffree;
    uint32_t f_fsid[2];
    uint64_t f_namelen;
    uint64_t f_frsize;
    uint64_t f_flags;
    uint64_t f_spare[4];
};

int statfs(const char *path, struct statfs *buf);
int fstatfs(int fd, struct statfs *buf);
