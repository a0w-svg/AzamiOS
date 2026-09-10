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

struct itimerspec {
    struct timespec it_interval;
    struct timespec it_value;
};


struct tm {
    int         tm_sec;    /* seconds [0, 60] */
    int         tm_min;    /* minutes [0, 59] */
    int         tm_hour;   /* hours [0, 23] */
    int         tm_mday;   /* day of the month [1, 31] */
    int         tm_mon;    /* month [0, 11] */
    int         tm_year;   /* years since 1900 */
    int         tm_wday;   /* day of the week [0, 6] (Sunday = 0) */
    int         tm_yday;   /* day in the year [0, 365] */
    int         tm_isdst;  /* daylight saving time flag */
    long        tm_gmtoff; /* seconds east of UTC */
    const char *tm_zone;   /* timezone abbreviation */
};

/* ── POSIX per-process timers ────────────────────────────────────────────── */

/* clockid_t and timer_t come from <sys/types.h>, already included above. */

/* timer_settime() flag: interpret it_value as an absolute clock reading. */
#define TIMER_ABSTIME 1

#ifndef __union_sigval_defined
#define __union_sigval_defined
union sigval {
    int   sival_int;
    void *sival_ptr;
};
#endif

#ifndef SIGEV_SIGNAL
struct sigevent {
    union sigval sigev_value;
    int          sigev_signo;
    int          sigev_notify;
    int          __pad[12];
};

#define SIGEV_SIGNAL 0
#define SIGEV_NONE   1
#define SIGEV_THREAD 2
#endif

/**
 * timer_create(clockid, sevp, timerid) — create a disarmed timer.
 *
 * @sevp may be NULL, which requests SIGALRM on expiry.  The timer does not
 * start counting until timer_settime().
 */
int timer_create(clockid_t clockid, struct sigevent *sevp, timer_t *timerid);

/** timer_settime(t, flags, new, old) — arm, re-arm, or (it_value == 0) disarm. */
int timer_settime(timer_t timerid, int flags,
                  const struct itimerspec *new_value, struct itimerspec *old_value);

/** timer_gettime(t, curr) — time remaining and the reload interval. */
int timer_gettime(timer_t timerid, struct itimerspec *curr_value);

/** timer_getoverrun(t) → expiries missed since the last delivery. */
int timer_getoverrun(timer_t timerid);

/** timer_delete(t) — destroy a timer, armed or not. */
int timer_delete(timer_t timerid);

#define CLOCK_REALTIME           0
#define CLOCK_MONOTONIC          1
#define CLOCK_PROCESS_CPUTIME_ID 2
#define CLOCK_THREAD_CPUTIME_ID  3

extern char *tzname[2];
extern long  timezone;
extern int   daylight;

void       tzset(void);
time_t     time(time_t *tloc);
int        clock_gettime(int clk_id, struct timespec *tp);
int        clock_getres(int clk_id, struct timespec *res);
int        clock_settime(int clk_id, const struct timespec *tp);
int        clock_nanosleep(int clock_id, int flags, const struct timespec *request, struct timespec *remain);
int        nanosleep(const struct timespec *req, struct timespec *rem);
double     difftime(time_t time1, time_t time0);
time_t     mktime(struct tm *timeptr);
time_t     timegm(struct tm *timeptr);
struct tm *gmtime(const time_t *timer);
struct tm *gmtime_r(const time_t *timer, struct tm *result);
struct tm *localtime(const time_t *timer);
struct tm *localtime_r(const time_t *timer, struct tm *result);
char      *asctime(const struct tm *timeptr);
char      *asctime_r(const struct tm *timeptr, char *buf);
char      *ctime(const time_t *timer);
char      *ctime_r(const time_t *timer, char *buf);
size_t     strftime(char *s, size_t maxsize, const char *format, const struct tm *timeptr);
