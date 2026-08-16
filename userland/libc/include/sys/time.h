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

int gettimeofday(struct timeval *tv, struct timezone *tz);
