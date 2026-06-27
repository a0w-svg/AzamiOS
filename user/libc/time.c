/**
 * time.c — AzamiOS libc: POSIX-compatible time functions
 */
#include "include/time.h"
#include "include/stdio.h"

/* Syscall wrapper for RTC time */
void rtc_get_time(rtc_time_t *t) {
    asm volatile("int $128" : : "a"(4), "b"(t) : "memory");
}

static int is_leap_year(int year) {
    return (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
}

static const int days_in_month[12] = {
    31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
};

time_t mktime(struct tm *tm) {
    if (!tm) return -1;
    long days = 0;
    int year = tm->tm_year + 1900;
    
    for (int y = 1970; y < year; y++) {
        days += is_leap_year(y) ? 366 : 365;
    }
    
    for (int m = 0; m < tm->tm_mon && m < 12; m++) {
        days += days_in_month[m];
        if (m == 1 && is_leap_year(year)) days++;
    }
    
    days += tm->tm_mday - 1;
    
    return (time_t)(days * 86400 + tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec);
}

time_t time(time_t *tloc) {
    rtc_time_t rt;
    rtc_get_time(&rt);
    struct tm t;
    t.tm_sec = rt.second;
    t.tm_min = rt.minute;
    t.tm_hour = rt.hour;
    t.tm_mday = rt.day;
    t.tm_mon = rt.month - 1;
    t.tm_year = rt.year - 1900;
    time_t res = mktime(&t);
    if (tloc) *tloc = res;
    return res;
}

static struct tm _static_tm;

struct tm *gmtime(const time_t *tp) {
    if (!tp) return 0;
    time_t t = *tp;
    int days = t / 86400;
    int rem = t % 86400;
    
    _static_tm.tm_hour = rem / 3600;
    rem %= 3600;
    _static_tm.tm_min = rem / 60;
    _static_tm.tm_sec = rem % 60;
    
    int year = 1970;
    while (1) {
        int ydays = is_leap_year(year) ? 366 : 365;
        if (days < ydays) break;
        days -= ydays;
        year++;
    }
    _static_tm.tm_year = year - 1900;
    _static_tm.tm_yday = days;
    _static_tm.tm_wday = (days + 4) % 7;
    
    int m = 0;
    while (m < 12) {
        int mdays = days_in_month[m];
        if (m == 1 && is_leap_year(year)) mdays++;
        if (days < mdays) break;
        days -= mdays;
        m++;
    }
    _static_tm.tm_mon = m;
    _static_tm.tm_mday = days + 1;
    _static_tm.tm_isdst = 0;
    
    return &_static_tm;
}

struct tm *localtime(const time_t *tp) {
    return gmtime(tp);
}

double difftime(time_t t1, time_t t0) {
    return (double)(t1 - t0);
}

clock_t clock(void) {
    return (clock_t)(time(0) * CLOCKS_PER_SEC);
}

size_t strftime(char *buf, size_t max, const char *fmt, const struct tm *tm) {
    if (!buf || !fmt || !tm || max == 0) return 0;
    return (size_t)snprintf(buf, max, "%04d-%02d-%02d %02d:%02d:%02d",
                            tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                            tm->tm_hour, tm->tm_min, tm->tm_sec);
}
