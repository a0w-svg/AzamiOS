/* ============================================================================
 * AzamiOS Userspace — Time Types and Declarations (time.h)
 * File: userland/libc/include/time.h
 * ============================================================================ */
#pragma once

#include "sys/types.h"
#include "sys/time.h"
#include <stddef.h>

struct timespec {
    time_t tv_sec;
    long   tv_nsec;
};

struct tm {
    int tm_sec;         /* seconds [0, 60] */
    int tm_min;         /* minutes [0, 59] */
    int tm_hour;        /* hours [0, 23] */
    int tm_mday;        /* day of the month [1, 31] */
    int tm_mon;         /* month [0, 11] */
    int tm_year;        /* years since 1900 */
    int tm_wday;        /* day of the week [0, 6] (Sunday = 0) */
    int tm_yday;        /* day in the year [0, 365] */
    int tm_isdst;       /* daylight saving time flag */
};

#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1

time_t     time(time_t *tloc);
int        clock_gettime(int clk_id, struct timespec *tp);
int        nanosleep(const struct timespec *req, struct timespec *rem);
double     difftime(time_t time1, time_t time0);
time_t     mktime(struct tm *timeptr);
struct tm *gmtime(const time_t *timer);
struct tm *localtime(const time_t *timer);
char      *asctime(const struct tm *timeptr);
char      *ctime(const time_t *timer);
size_t     strftime(char *s, size_t maxsize, const char *format, const struct tm *timeptr);
