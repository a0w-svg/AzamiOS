/* ============================================================================
 * AzamiOS Userspace — Standard Types Header (sys/types.h)
 * File: userland/libc/include/sys/types.h
 * ============================================================================ */
#pragma once

#include <stdint.h>

#ifndef NULL
#define NULL ((void *)0)
#endif

typedef unsigned long size_t;
typedef long          ssize_t;
typedef long          off_t;
typedef int           pid_t;
typedef unsigned int  mode_t;
typedef unsigned int  uid_t;
typedef unsigned int  gid_t;
typedef long          time_t;
typedef unsigned long clock_t;
typedef int           clockid_t;
typedef int           id_t;
typedef unsigned long ino_t;
typedef unsigned long dev_t;
typedef unsigned int  nlink_t;
typedef long          blksize_t;
typedef long          blkcnt_t;
typedef unsigned long uintptr_t;
typedef long          intptr_t;
typedef int           key_t;
typedef unsigned int  useconds_t;
typedef long          suseconds_t;
typedef char         *caddr_t;
typedef long          daddr_t;
typedef unsigned long fsblkcnt_t;
typedef unsigned long fsfilcnt_t;
typedef int           timer_t;
