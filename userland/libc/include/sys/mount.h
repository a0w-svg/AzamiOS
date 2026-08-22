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
#define MS_NOATIME     1024
#define MS_NODIRATIME  2048
#define MS_BIND        4096

int mount(const char *specialfile, const char *dir,
          const char *filesystemtype, unsigned long rwflag,
          const void *data);
int umount(const char *dir);
int umount2(const char *dir, int flags);
