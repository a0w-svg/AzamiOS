/**
 * time.h — AzamiOS libc: POSIX-compatible time types and functions
 */
#ifndef _TIME_H
#define _TIME_H

#include <stdint.h>
#include <stddef.h>

/* time_t: seconds since 1970-01-01 00:00:00 UTC */
typedef int32_t time_t;
typedef int32_t clock_t;

#define CLOCKS_PER_SEC 1000

/* Hardware RTC structure returned by sys_time / rtc_get_time */
typedef struct {
    uint8_t second;
    uint8_t minute;
    uint8_t hour;
    uint8_t day;
    uint8_t month;
    uint32_t year;
} __attribute__((packed)) rtc_time_t;

/* Broken-down calendar time (POSIX struct tm) */
struct tm {
    int tm_sec;    /* Seconds       [0,60]  */
    int tm_min;    /* Minutes       [0,59]  */
    int tm_hour;   /* Hours         [0,23]  */
    int tm_mday;   /* Day of month  [1,31]  */
    int tm_mon;    /* Month         [0,11]  */
    int tm_year;   /* Year - 1900           */
    int tm_wday;   /* Day of week   [0,6] (Sunday = 0) */
    int tm_yday;   /* Day of year   [0,365] */
    int tm_isdst;  /* DST flag (-1/0/1)     */
};

/* RTC hardware read */
void rtc_get_time(rtc_time_t *t);

/* POSIX time API */
time_t time      (time_t *tloc);
struct tm *localtime(const time_t *tp);
struct tm *gmtime   (const time_t *tp);
time_t mktime    (struct tm *tm);
double difftime  (time_t t1, time_t t0);
clock_t clock    (void);

/* Formatting */
size_t strftime  (char *buf, size_t max, const char *fmt, const struct tm *tm);

#endif /* _TIME_H */
