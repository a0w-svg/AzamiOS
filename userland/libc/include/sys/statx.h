/* ============================================================================
 * AzamiOS Userspace — Linux Extended File Status Header (sys/statx.h)
 * File: userland/libc/include/sys/statx.h
 * ============================================================================ */
#pragma once

#include <stdint.h>
#include <sys/types.h>
#include <sys/syscall.h> /* struct statx/statx_timestamp, STATX_* — canonical uapi */

/* `struct statx`, `struct statx_timestamp`, and STATX_TYPE..STATX_ALL all
 * come from <sys/syscall.h>'s canonical uapi header
 * (include/azami/uapi/syscall_nr.h) — this used to be a second,
 * hand-duplicated copy of both, which is exactly the kind of drift
 * scripts/check_uapi_sync.sh exists to catch. */

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
