/* ============================================================================
 * AzamiOS Userspace — Time Implementation (time.c)
 * File: userland/libc/time.c
 * ============================================================================ */

#include "include/time.h"
#include "include/stdio.h"
#include "include/string.h"

static const int g_days_per_month[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
static const char *g_wday_names[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *g_mon_names[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

static inline int is_leap_year(int year)
{
    return (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
}

time_t time(time_t *tloc)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) == 0) {
        if (tloc) *tloc = ts.tv_sec;
        return ts.tv_sec;
    }
    time_t t = 1786874400; /* Aug 2026 fallback */
    if (tloc) *tloc = t;
    return t;
}

double difftime(time_t time1, time_t time0)
{
    return (double)(time1 - time0);
}

static struct tm g_static_tm;

struct tm *gmtime(const time_t *timer)
{
    if (!timer) return NULL;
    time_t t = *timer;
    if (t < 0) t = 0;

    g_static_tm.tm_sec = (int)(t % 60);
    t /= 60;
    g_static_tm.tm_min = (int)(t % 60);
    t /= 60;
    g_static_tm.tm_hour = (int)(t % 24);
    time_t days = t / 24;

    /* Day of week (Jan 1, 1970 was Thursday = 4) */
    g_static_tm.tm_wday = (int)((days + 4) % 7);

    int year = 1970;
    while (1) {
        int days_in_year = is_leap_year(year) ? 366 : 365;
        if (days < days_in_year) break;
        days -= days_in_year;
        year++;
    }
    g_static_tm.tm_year = year - 1900;
    g_static_tm.tm_yday = (int)days;

    int mon = 0;
    while (mon < 12) {
        int dim = g_days_per_month[mon];
        if (mon == 1 && is_leap_year(year)) dim = 29;
        if (days < dim) break;
        days -= dim;
        mon++;
    }
    g_static_tm.tm_mon = mon;
    g_static_tm.tm_mday = (int)(days + 1);
    g_static_tm.tm_isdst = 0;

    return &g_static_tm;
}

struct tm *localtime(const time_t *timer)
{
    return gmtime(timer); /* UTC default without timezone db */
}

time_t mktime(struct tm *timeptr)
{
    if (!timeptr) return (time_t)-1;
    int year = timeptr->tm_year + 1900;
    time_t days = 0;

    for (int y = 1970; y < year; y++) {
        days += is_leap_year(y) ? 366 : 365;
    }
    for (int m = 0; m < timeptr->tm_mon && m < 12; m++) {
        days += g_days_per_month[m];
        if (m == 1 && is_leap_year(year)) days++;
    }
    days += (timeptr->tm_mday - 1);

    time_t seconds = days * 86400;
    seconds += timeptr->tm_hour * 3600;
    seconds += timeptr->tm_min * 60;
    seconds += timeptr->tm_sec;
    return seconds;
}

static char g_asctime_buf[32];

char *asctime(const struct tm *timeptr)
{
    if (!timeptr) return NULL;
    int wday = (timeptr->tm_wday >= 0 && timeptr->tm_wday < 7) ? timeptr->tm_wday : 0;
    int mon  = (timeptr->tm_mon >= 0 && timeptr->tm_mon < 12) ? timeptr->tm_mon : 0;

    snprintf(g_asctime_buf, sizeof(g_asctime_buf), "%s %s %2d %02d:%02d:%02d %d\n",
             g_wday_names[wday], g_mon_names[mon],
             timeptr->tm_mday, timeptr->tm_hour, timeptr->tm_min, timeptr->tm_sec,
             timeptr->tm_year + 1900);
    return g_asctime_buf;
}

char *ctime(const time_t *timer)
{
    return asctime(localtime(timer));
}

size_t strftime(char *s, size_t maxsize, const char *format, const struct tm *timeptr)
{
    if (!s || maxsize == 0 || !format || !timeptr) return 0;
    char temp[64];
    size_t out_len = 0;
    s[0] = '\0';

    while (*format && out_len < maxsize - 1) {
        if (*format != '%') {
            s[out_len++] = *format++;
            continue;
        }
        format++;
        temp[0] = '\0';
        switch (*format) {
            case 'Y': snprintf(temp, sizeof(temp), "%04d", timeptr->tm_year + 1900); break;
            case 'm': snprintf(temp, sizeof(temp), "%02d", timeptr->tm_mon + 1); break;
            case 'd': snprintf(temp, sizeof(temp), "%02d", timeptr->tm_mday); break;
            case 'H': snprintf(temp, sizeof(temp), "%02d", timeptr->tm_hour); break;
            case 'M': snprintf(temp, sizeof(temp), "%02d", timeptr->tm_min); break;
            case 'S': snprintf(temp, sizeof(temp), "%02d", timeptr->tm_sec); break;
            case 'a': strncpy(temp, g_wday_names[timeptr->tm_wday % 7], sizeof(temp)); break;
            case 'b': strncpy(temp, g_mon_names[timeptr->tm_mon % 12], sizeof(temp)); break;
            case '%': temp[0] = '%'; temp[1] = '\0'; break;
            default:  temp[0] = *format; temp[1] = '\0'; break;
        }
        format++;
        size_t tlen = strlen(temp);
        for (size_t i = 0; i < tlen && out_len < maxsize - 1; i++) {
            s[out_len++] = temp[i];
        }
    }
    s[out_len] = '\0';
    return out_len;
}
