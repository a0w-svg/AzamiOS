/* ============================================================================
 * AzamiOS Userspace — Filesystem Mount Header (sys/mount.h)
 * File: userland/libc/include/sys/mount.h
 * ============================================================================ */
#pragma once

#define MS_RDONLY      1
#define MS_NOSUID      2
#define MS_NODEV       4
#define MS_NOEXEC      8
#define MS_SYNCHRONOUS 16
#define MS_REMOUNT     32
#define MS_MANDLOCK    64
#define MS_DIRSYNC     128
#define MS_NOSYMFOLLOW 256
#define MS_NOATIME     1024
#define MS_NODIRATIME  2048
#define MS_BIND        4096
#define MS_MOVE        8192
#define MS_REC         16384
#define MS_SILENT      32768
#define MS_POSIXACL    (1 << 16)
#define MS_UNBINDABLE  (1 << 17)
#define MS_PRIVATE     (1 << 18)
#define MS_SLAVE       (1 << 19)
#define MS_SHARED      (1 << 20)
#define MS_RELATIME    (1 << 21)
#define MS_I_VERSION   (1 << 23)
#define MS_STRICTATIME (1 << 24)
#define MS_LAZYTIME    (1 << 25)

/* The magic word pre-2.4 mount(2) wanted in the top 16 bits of the flags. */
#define MS_MGC_VAL     0xC0ED0000U
#define MS_MGC_MSK     0xFFFF0000U

/* umount2(2) flags */
#define MNT_FORCE       0x00000001
#define MNT_DETACH      0x00000002
#define MNT_EXPIRE      0x00000004
#define UMOUNT_NOFOLLOW 0x00000008

int mount(const char *specialfile, const char *dir,
          const char *filesystemtype, unsigned long rwflag,
          const void *data);
int umount(const char *dir);
int umount2(const char *dir, int flags);
