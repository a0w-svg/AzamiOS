/* ============================================================================
 * AzamiOS Userspace — Time Types and Functions (sys/time.h)
 * File: userland/libc/include/sys/time.h
 * ============================================================================ */
#pragma once

#include "types.h"

struct timeval {
    time_t tv_sec;
    long   tv_usec;
};

struct timezone {
    int tz_minuteswest;
    int tz_dsttime;
};

struct itimerval {
    struct timeval it_interval; /* Interval for periodic timer */
    struct timeval it_value;    /* Time until next expiration */
};

#define ITIMER_REAL    0
#define ITIMER_VIRTUAL 1
#define ITIMER_PROF    2

int gettimeofday(struct timeval *tv, struct timezone *tz);
int settimeofday(const struct timeval *tv, const struct timezone *tz);
int getitimer(int which, struct itimerval *curr_value);
int setitimer(int which, const struct itimerval *new_value, struct itimerval *old_value);
int utimes(const char *filename, const struct timeval times[2]);
