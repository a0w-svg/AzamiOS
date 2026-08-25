/* ============================================================================
 * AzamiOS Userspace — Linux Timerfd Subsystem Header (sys/timerfd.h)
 * File: userland/libc/include/sys/timerfd.h
 * ============================================================================ */
#pragma once

#include <time.h>
#include <stdint.h>

#define TFD_TIMER_ABSTIME (1 << 0)
#define TFD_TIMER_CANCEL_ON_SET (1 << 1)
#define TFD_CLOEXEC       02000000
#define TFD_NONBLOCK      00004000

int timerfd_create(int clockid, int flags);
int timerfd_settime(int fd, int flags, const struct itimerspec *new_value, struct itimerspec *old_value);
int timerfd_gettime(int fd, struct itimerspec *curr_value);
