/* ============================================================================
 * AzamiOS Userspace — File System Statistics Header (sys/statvfs.h)
 * File: userland/libc/include/sys/statvfs.h
 * ============================================================================ */
#pragma once

#include "types.h"

typedef unsigned long fsblkcnt_t;
typedef unsigned long fsfilcnt_t;

struct statvfs {
    unsigned long f_bsize;   /* File system block size */
    unsigned long f_frsize;  /* Fragment size */
    fsblkcnt_t    f_blocks;  /* Size of fs in f_frsize units */
    fsblkcnt_t    f_bfree;   /* Free blocks */
    fsblkcnt_t    f_bavail;  /* Free blocks for non-root */
    fsfilcnt_t    f_files;   /* Total file nodes (inodes) */
    fsfilcnt_t    f_ffree;   /* Free file nodes */
    fsfilcnt_t    f_favail;  /* Free file nodes for non-root */
    unsigned long f_fsid;    /* File system ID */
    unsigned long f_flag;    /* Mount flags */
    unsigned long f_namemax; /* Maximum filename length */
};

#define ST_RDONLY 1
#define ST_NOSUID 2
#define ST_NODEV  4
#define ST_NOEXEC 8

int statvfs(const char *path, struct statvfs *buf);
int fstatvfs(int fd, struct statvfs *buf);
