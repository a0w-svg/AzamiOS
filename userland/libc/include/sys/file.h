/* ============================================================================
 * AzamiOS Userspace — File Operations & Locking (sys/file.h)
 * File: userland/libc/include/sys/file.h
 * ============================================================================ */
#pragma once

#include "types.h"

#define LOCK_SH 1   /* Shared lock */
#define LOCK_EX 2   /* Exclusive lock */
#define LOCK_NB 4   /* Don't block when locking */
#define LOCK_UN 8   /* Unlock */

/* Legacy BSD/POSIX seek aliases */
#define L_SET   0   /* SEEK_SET */
#define L_INCR  1   /* SEEK_CUR */
#define L_XTND  2   /* SEEK_END */

int flock(int fd, int operation);
