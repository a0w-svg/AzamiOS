/* ============================================================================
 * AzamiOS Userspace — Linux Extended File Status Header (sys/statx.h)
 * File: userland/libc/include/sys/statx.h
 * ============================================================================ */
#pragma once

#include <stdint.h>
#include <sys/types.h>

struct statx_timestamp {
    int64_t  tv_sec;
    uint32_t tv_nsec;
    int32_t  __reserved;
};

struct statx {
    uint32_t stx_mask;
    uint32_t stx_blksize;
    uint64_t stx_attributes;
    uint32_t stx_nlink;
    uint32_t stx_uid;
    uint32_t stx_gid;
    uint16_t stx_mode;
    uint16_t __spare0[1];
    uint64_t stx_ino;
    uint64_t stx_size;
    uint64_t stx_blocks;
    uint64_t stx_attributes_mask;
    struct statx_timestamp stx_atime;
    struct statx_timestamp stx_btime;
    struct statx_timestamp stx_ctime;
    struct statx_timestamp stx_mtime;
    uint32_t stx_rdev_major;
    uint32_t stx_rdev_minor;
    uint32_t stx_dev_major;
    uint32_t stx_dev_minor;
    uint64_t stx_mnt_id;
    uint64_t __spare2;
    uint64_t __spare3[12];
};

#define STATX_TYPE          0x00000001U
#define STATX_MODE          0x00000002U
#define STATX_NLINK         0x00000004U
#define STATX_UID           0x00000008U
#define STATX_GID           0x00000010U
#define STATX_ATIME         0x00000020U
#define STATX_MTIME         0x00000040U
#define STATX_CTIME         0x00000080U
#define STATX_INO           0x00000100U
#define STATX_SIZE          0x00000200U
#define STATX_BLOCKS        0x00000400U
#define STATX_BASIC_STATS   0x000007ffU
#define STATX_BTIME         0x00000800U
#define STATX_ALL           0x00000fffU

#define STATX_ATTR_COMPRESSED 0x00000004
#define STATX_ATTR_IMMUTABLE  0x00000010
#define STATX_ATTR_APPEND     0x00000020
#define STATX_ATTR_NODUMP     0x00000040
#define STATX_ATTR_ENCRYPTED  0x00000800
#define STATX_ATTR_AUTOMOUNT  0x00001000
#define STATX_ATTR_MOUNT_ROOT 0x00002000
#define STATX_ATTR_VERITY     0x00100000
#define STATX_ATTR_DAX        0x00200000

#ifndef AT_STATX_SYNC_AS_STAT
#define AT_STATX_SYNC_AS_STAT 0x0000
#endif
#ifndef AT_STATX_FORCE_SYNC
#define AT_STATX_FORCE_SYNC   0x2000
#endif
#ifndef AT_STATX_DONT_SYNC
#define AT_STATX_DONT_SYNC    0x4000
#endif

int statx(int dirfd, const char *pathname, int flags, unsigned int mask, struct statx *statxbuf);
