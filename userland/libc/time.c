/* ============================================================================
 * AzamiOS Userspace — Time & Timezone Implementation (time.c)
 * File: userland/libc/time.c
 * ============================================================================ */

#include "include/time.h"
#include "include/stdio.h"
#include "include/stdlib.h"
#include "include/string.h"
#include "include/unistd.h"
#include "include/ctype.h"
#include "include/sys/syscall.h"

static const int g_days_per_month[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
static const char *g_wday_names_short[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *g_wday_names_long[7]  = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
static const char *g_mon_names_short[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
static const char *g_mon_names_long[12]  = { "January", "February", "March", "April", "May", "June",
                                             "July", "August", "September", "October", "November", "December" };

/* POSIX Timezone Variables */
char *tzname[2] = { "UTC", "UTC" };
long timezone = 0;   /* seconds west of UTC */
int daylight = 0;    /* 1 if DST is used */

static char g_tz_std[16] = "UTC";
static char g_tz_dst[16] = "UTC";
static long g_tz_offset_std = 0; /* seconds east of UTC */
static long g_tz_offset_dst = 0;
static int  g_tz_has_dst = 0;
static int  g_tz_is_southern = 0;
static int  g_tz_initialized = 0;

typedef struct {
    const char *name;
    const char *std_abbr;
    const char *dst_abbr;
    long std_offset; /* seconds east of UTC */
    long dst_offset;
    int has_dst;
    int is_southern;
} tz_entry_t;

static const tz_entry_t g_tz_db[] = {
    { "UTC", "UTC", "UTC", 0, 0, 0, 0 },
    { "GMT", "GMT", "GMT", 0, 0, 0, 0 },
    { "Europe/London", "GMT", "BST", 0, 3600, 1, 0 },
    { "Europe/Paris", "CET", "CEST", 3600, 7200, 1, 0 },
    { "Europe/Berlin", "CET", "CEST", 3600, 7200, 1, 0 },
    { "Europe/Rome", "CET", "CEST", 3600, 7200, 1, 0 },
    { "Europe/Madrid", "CET", "CEST", 3600, 7200, 1, 0 },
    { "Europe/Amsterdam", "CET", "CEST", 3600, 7200, 1, 0 },
    { "Europe/Brussels", "CET", "CEST", 3600, 7200, 1, 0 },
    { "Europe/Vienna", "CET", "CEST", 3600, 7200, 1, 0 },
    { "Europe/Stockholm", "CET", "CEST", 3600, 7200, 1, 0 },
    { "Europe/Oslo", "CET", "CEST", 3600, 7200, 1, 0 },
    { "Europe/Copenhagen", "CET", "CEST", 3600, 7200, 1, 0 },
    { "Europe/Warsaw", "CET", "CEST", 3600, 7200, 1, 0 },
    { "Europe/Prague", "CET", "CEST", 3600, 7200, 1, 0 },
    { "Europe/Athens", "EET", "EEST", 7200, 10800, 1, 0 },
    { "Europe/Helsinki", "EET", "EEST", 7200, 10800, 1, 0 },
    { "Europe/Kyiv", "EET", "EEST", 7200, 10800, 1, 0 },
    { "Europe/Bucharest", "EET", "EEST", 7200, 10800, 1, 0 },
    { "Europe/Moscow", "MSK", "MSK", 10800, 10800, 0, 0 },
    { "CET", "CET", "CEST", 3600, 7200, 1, 0 },
    { "CEST", "CEST", "CEST", 7200, 7200, 0, 0 },
    { "EET", "EET", "EEST", 7200, 10800, 1, 0 },
    { "EEST", "EEST", "EEST", 10800, 10800, 0, 0 },
    { "America/New_York", "EST", "EDT", -18000, -14400, 1, 0 },
    { "America/Detroit", "EST", "EDT", -18000, -14400, 1, 0 },
    { "America/Toronto", "EST", "EDT", -18000, -14400, 1, 0 },
    { "America/Chicago", "CST", "CDT", -21600, -18000, 1, 0 },
    { "America/Denver", "MST", "MDT", -25200, -21600, 1, 0 },
    { "America/Phoenix", "MST", "MST", -25200, -25200, 0, 0 },
    { "America/Los_Angeles", "PST", "PDT", -28800, -25200, 1, 0 },
    { "America/Vancouver", "PST", "PDT", -28800, -25200, 1, 0 },
    { "America/Anchorage", "AKST", "AKDT", -32400, -28800, 1, 0 },
    { "Pacific/Honolulu", "HST", "HST", -36000, -36000, 0, 0 },
    { "America/Halifax", "AST", "ADT", -14400, -10800, 1, 0 },
    { "America/Sao_Paulo", "BRT", "BRT", -10800, -10800, 0, 1 },
    { "America/Buenos_Aires", "ART", "ART", -10800, -10800, 0, 1 },
    { "EST", "EST", "EDT", -18000, -14400, 1, 0 },
    { "EDT", "EDT", "EDT", -14400, -14400, 0, 0 },
    { "CST", "CST", "CDT", -21600, -18000, 1, 0 },
    { "CDT", "CDT", "CDT", -18000, -18000, 0, 0 },
    { "MST", "MST", "MDT", -25200, -21600, 1, 0 },
    { "MDT", "MDT", "MDT", -21600, -21600, 0, 0 },
    { "PST", "PST", "PDT", -28800, -25200, 1, 0 },
    { "PDT", "PDT", "PDT", -25200, -25200, 0, 0 },
    { "Asia/Dubai", "GST", "GST", 14400, 14400, 0, 0 },
    { "Asia/Karachi", "PKT", "PKT", 18000, 18000, 0, 0 },
    { "Asia/Kolkata", "IST", "IST", 19800, 19800, 0, 0 },
    { "Asia/Calcutta", "IST", "IST", 19800, 19800, 0, 0 },
    { "Asia/Bangkok", "ICT", "ICT", 25200, 25200, 0, 0 },
    { "Asia/Jakarta", "WIB", "WIB", 25200, 25200, 0, 0 },
    { "Asia/Singapore", "SGT", "SGT", 28800, 28800, 0, 0 },
    { "Asia/Shanghai", "CST", "CST", 28800, 28800, 0, 0 },
    { "Asia/Hong_Kong", "HKT", "HKT", 28800, 28800, 0, 0 },
    { "Asia/Taipei", "CST", "CST", 28800, 28800, 0, 0 },
    { "Asia/Tokyo", "JST", "JST", 32400, 32400, 0, 0 },
    { "Asia/Seoul", "KST", "KST", 32400, 32400, 0, 0 },
    { "Australia/Perth", "AWST", "AWST", 28800, 28800, 0, 1 },
    { "Australia/Adelaide", "ACST", "ACDT", 34200, 37800, 1, 1 },
    { "Australia/Darwin", "ACST", "ACST", 34200, 34200, 0, 1 },
    { "Australia/Sydney", "AEST", "AEDT", 36000, 39600, 1, 1 },
    { "Australia/Melbourne", "AEST", "AEDT", 36000, 39600, 1, 1 },
    { "Australia/Brisbane", "AEST", "AEST", 36000, 36000, 0, 1 },
    { "Pacific/Auckland", "NZST", "NZDT", 43200, 46800, 1, 1 },
};
#define NUM_TZ_DB ((int)(sizeof(g_tz_db)/sizeof(g_tz_db[0])))

static inline int is_leap_year(int year)
{
    return (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
}

static int parse_numeric_tz(const char *tz_str, long *out_offset, char *out_name, size_t name_sz)
{
    const char *p = tz_str;
    if (strncmp(p, "UTC", 3) == 0 || strncmp(p, "GMT", 3) == 0) {
        p += 3;
    }
    if (*p != '+' && *p != '-') return 0;

    int sign = (*p == '-') ? -1 : 1;
    p++;

    int hours = 0;
    int mins = 0;
    while (*p && isdigit(*p) && hours < 100) {
        hours = hours * 10 + (*p - '0');
        p++;
    }
    if (*p == ':') {
        p++;
        while (*p && isdigit(*p) && mins < 100) {
            mins = mins * 10 + (*p - '0');
            p++;
        }
    } else if (hours >= 100) {
        /* Format +0200 or -0500 */
        mins = hours % 100;
        hours = hours / 100;
    }

    *out_offset = (long)(hours * 3600 + mins * 60) * sign;
    if (mins > 0) {
        snprintf(out_name, name_sz, "UTC%c%d:%02d", (sign < 0 ? '-' : '+'), hours, mins);
    } else {
        snprintf(out_name, name_sz, "UTC%c%d", (sign < 0 ? '-' : '+'), hours);
    }
    return 1;
}

void tzset(void)
{
    g_tz_initialized = 1;
    char tz_buf[64] = "";

    const char *env_tz = getenv("TZ");
    if (env_tz && env_tz[0] != '\0') {
        strncpy(tz_buf, env_tz, sizeof(tz_buf) - 1);
    } else {
        /* Check /hdd/etc/timezone, then fallback to /etc/timezone */
        int fd = sys_open("/hdd/etc/timezone", 0, 0);
        if (fd < 0) {
            fd = sys_open("/etc/timezone", 0, 0);
        }
        if (fd >= 0) {
            int n = (int)sys_read(fd, tz_buf, sizeof(tz_buf) - 1);
            sys_close(fd);
            if (n > 0) {
                tz_buf[n] = '\0';
                /* Strip trailing whitespace/newlines */
                while (n > 0 && (tz_buf[n - 1] == '\r' || tz_buf[n - 1] == '\n' || tz_buf[n - 1] == ' ')) {
                    tz_buf[--n] = '\0';
                }
            }
        }
    }

    /* Fallback to UTC if empty */
    if (tz_buf[0] == '\0') {
        strcpy(tz_buf, "UTC");
    }

    /* 1. Try matching database */
    for (int i = 0; i < NUM_TZ_DB; i++) {
        if (strcmp(tz_buf, g_tz_db[i].name) == 0) {
            strncpy(g_tz_std, g_tz_db[i].std_abbr, sizeof(g_tz_std) - 1);
            strncpy(g_tz_dst, g_tz_db[i].dst_abbr, sizeof(g_tz_dst) - 1);
            g_tz_offset_std = g_tz_db[i].std_offset;
            g_tz_offset_dst = g_tz_db[i].dst_offset;
            g_tz_has_dst = g_tz_db[i].has_dst;
            g_tz_is_southern = g_tz_db[i].is_southern;

            tzname[0] = g_tz_std;
            tzname[1] = g_tz_dst;
            timezone = -g_tz_offset_std;
            daylight = g_tz_has_dst;
            return;
        }
    }

    /* 2. Try parsing numeric offset (e.g. UTC+2, +02:00, -0500) */
    long num_offset = 0;
    char num_name[16] = "";
    if (parse_numeric_tz(tz_buf, &num_offset, num_name, sizeof(num_name))) {
        strncpy(g_tz_std, num_name, sizeof(g_tz_std) - 1);
        strncpy(g_tz_dst, num_name, sizeof(g_tz_dst) - 1);
        g_tz_offset_std = num_offset;
        g_tz_offset_dst = num_offset;
        g_tz_has_dst = 0;
        g_tz_is_southern = 0;

        tzname[0] = g_tz_std;
        tzname[1] = g_tz_dst;
        timezone = -g_tz_offset_std;
        daylight = 0;
        return;
    }

    /* 3. Default to UTC */
    strcpy(g_tz_std, "UTC");
    strcpy(g_tz_dst, "UTC");
    g_tz_offset_std = 0;
    g_tz_offset_dst = 0;
    g_tz_has_dst = 0;
    g_tz_is_southern = 0;

    tzname[0] = g_tz_std;
    tzname[1] = g_tz_dst;
    timezone = 0;
    daylight = 0;
}

static int check_is_dst(int year, int mon, int mday, int wday, int is_southern)
{
    (void)year;
    if (is_southern) {
        if (mon >= 9 || mon <= 2) return 1; /* Oct..Mar */
        return 0;
    }
    /* Northern hemisphere: March through October */
    if (mon > 2 && mon < 9) return 1; /* Apr (3) through Sep (8) */
    if (mon < 2 || mon > 9) return 0; /* Jan, Feb, Nov, Dec */
    if (mon == 2) { /* March */
        int last_sunday = 31 - ((wday + 31 - mday) % 7);
        return (mday >= last_sunday);
    }
    if (mon == 9) { /* October */
        int last_sunday = 31 - ((wday + 31 - mday) % 7);
        return (mday < last_sunday);
    }
    return 0;
}

time_t time(time_t *tloc)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) == 0) {
        if (tloc) *tloc = ts.tv_sec;
        return ts.tv_sec;
    }
    time_t t = 1787077600; /* Aug 2026 fallback */
    if (tloc) *tloc = t;
    return t;
}

double difftime(time_t time1, time_t time0)
{
    return (double)(time1 - time0);
}

static void break_down_seconds(time_t t, struct tm *result)
{
    if (t < 0) t = 0;

    result->tm_sec = (int)(t % 60);
    t /= 60;
    result->tm_min = (int)(t % 60);
    t /= 60;
    result->tm_hour = (int)(t % 24);
    time_t days = t / 24;

    /* Jan 1, 1970 was Thursday (wday = 4) */
    result->tm_wday = (int)((days + 4) % 7);

    int year = 1970;
    while (1) {
        int days_in_year = is_leap_year(year) ? 366 : 365;
        if (days < days_in_year) break;
        days -= days_in_year;
        year++;
    }
    result->tm_year = year - 1900;
    result->tm_yday = (int)days;

    int mon = 0;
    while (mon < 12) {
        int dim = g_days_per_month[mon];
        if (mon == 1 && is_leap_year(year)) dim = 29;
        if (days < dim) break;
        days -= dim;
        mon++;
    }
    result->tm_mon = mon;
    result->tm_mday = (int)(days + 1);
}

struct tm *gmtime_r(const time_t *timer, struct tm *result)
{
    if (!timer || !result) return NULL;
    break_down_seconds(*timer, result);
    result->tm_isdst = 0;
    result->tm_gmtoff = 0;
    result->tm_zone = "UTC";
    return result;
}

static struct tm g_static_gm_tm;
struct tm *gmtime(const time_t *timer)
{
    return gmtime_r(timer, &g_static_gm_tm);
}

struct tm *localtime_r(const time_t *timer, struct tm *result)
{
    if (!timer || !result) return NULL;
    if (!g_tz_initialized) tzset();

    time_t t = *timer;
    /* Estimate date components with standard offset to compute DST */
    struct tm temp;
    break_down_seconds(t + g_tz_offset_std, &temp);

    int is_dst = 0;
    if (g_tz_has_dst) {
        is_dst = check_is_dst(temp.tm_year + 1900, temp.tm_mon, temp.tm_mday, temp.tm_wday, g_tz_is_southern);
    }

    long active_offset = (is_dst && g_tz_has_dst) ? g_tz_offset_dst : g_tz_offset_std;
    break_down_seconds(t + active_offset, result);

    result->tm_isdst = is_dst;
    result->tm_gmtoff = active_offset;
    result->tm_zone = (is_dst && g_tz_has_dst) ? tzname[1] : tzname[0];

    return result;
}

static struct tm g_static_local_tm;
struct tm *localtime(const time_t *timer)
{
    return localtime_r(timer, &g_static_local_tm);
}

time_t timegm(struct tm *timeptr)
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

time_t mktime(struct tm *timeptr)
{
    if (!timeptr) return (time_t)-1;
    if (!g_tz_initialized) tzset();

    time_t utc_sec = timegm(timeptr);
    long offset = g_tz_offset_std;
    if (timeptr->tm_isdst > 0 && g_tz_has_dst) {
        offset = g_tz_offset_dst;
    }
    return utc_sec - offset;
}

char *asctime_r(const struct tm *timeptr, char *buf)
{
    if (!timeptr || !buf) return NULL;
    int wday = (timeptr->tm_wday >= 0 && timeptr->tm_wday < 7) ? timeptr->tm_wday : 0;
    int mon  = (timeptr->tm_mon >= 0 && timeptr->tm_mon < 12) ? timeptr->tm_mon : 0;

    sprintf(buf, "%s %s %2d %02d:%02d:%02d %d\n",
            g_wday_names_short[wday], g_mon_names_short[mon],
            timeptr->tm_mday, timeptr->tm_hour, timeptr->tm_min, timeptr->tm_sec,
            timeptr->tm_year + 1900);
    return buf;
}

static char g_asctime_buf[64];
char *asctime(const struct tm *timeptr)
{
    return asctime_r(timeptr, g_asctime_buf);
}

char *ctime_r(const time_t *timer, char *buf)
{
    struct tm tm_res;
    if (!localtime_r(timer, &tm_res)) return NULL;
    return asctime_r(&tm_res, buf);
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

    int hour12 = timeptr->tm_hour % 12;
    if (hour12 == 0) hour12 = 12;
    const char *ampm_upper = (timeptr->tm_hour >= 12) ? "PM" : "AM";
    const char *ampm_lower = (timeptr->tm_hour >= 12) ? "pm" : "am";

    while (*format && out_len < maxsize - 1) {
        if (*format != '%') {
            s[out_len++] = *format++;
            continue;
        }
        format++;
        temp[0] = '\0';

        switch (*format) {
            case 'Y': snprintf(temp, sizeof(temp), "%04d", timeptr->tm_year + 1900); break;
            case 'y': snprintf(temp, sizeof(temp), "%02d", (timeptr->tm_year + 1900) % 100); break;
            case 'm': snprintf(temp, sizeof(temp), "%02d", timeptr->tm_mon + 1); break;
            case 'd': snprintf(temp, sizeof(temp), "%02d", timeptr->tm_mday); break;
            case 'e': snprintf(temp, sizeof(temp), "%2d", timeptr->tm_mday); break;
            case 'H': snprintf(temp, sizeof(temp), "%02d", timeptr->tm_hour); break;
            case 'k': snprintf(temp, sizeof(temp), "%2d", timeptr->tm_hour); break;
            case 'I': snprintf(temp, sizeof(temp), "%02d", hour12); break;
            case 'l': snprintf(temp, sizeof(temp), "%2d", hour12); break;
            case 'M': snprintf(temp, sizeof(temp), "%02d", timeptr->tm_min); break;
            case 'S': snprintf(temp, sizeof(temp), "%02d", timeptr->tm_sec); break;
            case 'p': strncpy(temp, ampm_upper, sizeof(temp)); break;
            case 'P': strncpy(temp, ampm_lower, sizeof(temp)); break;
            case 'a': strncpy(temp, g_wday_names_short[timeptr->tm_wday % 7], sizeof(temp)); break;
            case 'A': strncpy(temp, g_wday_names_long[timeptr->tm_wday % 7], sizeof(temp)); break;
            case 'b':
            case 'h': strncpy(temp, g_mon_names_short[timeptr->tm_mon % 12], sizeof(temp)); break;
            case 'B': strncpy(temp, g_mon_names_long[timeptr->tm_mon % 12], sizeof(temp)); break;
            case 'w': snprintf(temp, sizeof(temp), "%d", timeptr->tm_wday % 7); break;
            case 'u': snprintf(temp, sizeof(temp), "%d", (timeptr->tm_wday == 0) ? 7 : timeptr->tm_wday); break;
            case 'j': snprintf(temp, sizeof(temp), "%03d", timeptr->tm_yday + 1); break;
            case 'F': snprintf(temp, sizeof(temp), "%04d-%02d-%02d", timeptr->tm_year + 1900, timeptr->tm_mon + 1, timeptr->tm_mday); break;
            case 'T':
            case 'X': snprintf(temp, sizeof(temp), "%02d:%02d:%02d", timeptr->tm_hour, timeptr->tm_min, timeptr->tm_sec); break;
            case 'R': snprintf(temp, sizeof(temp), "%02d:%02d", timeptr->tm_hour, timeptr->tm_min); break;
            case 'r': snprintf(temp, sizeof(temp), "%02d:%02d:%02d %s", hour12, timeptr->tm_min, timeptr->tm_sec, ampm_upper); break;
            case 'D':
            case 'x': snprintf(temp, sizeof(temp), "%02d/%02d/%02d", timeptr->tm_mon + 1, timeptr->tm_mday, (timeptr->tm_year + 1900) % 100); break;
            case 'c': snprintf(temp, sizeof(temp), "%s %s %2d %02d:%02d:%02d %s %d",
                               g_wday_names_short[timeptr->tm_wday % 7],
                               g_mon_names_short[timeptr->tm_mon % 12],
                               timeptr->tm_mday, timeptr->tm_hour, timeptr->tm_min, timeptr->tm_sec,
                               timeptr->tm_zone ? timeptr->tm_zone : "UTC",
                               timeptr->tm_year + 1900); break;
            case 'Z': strncpy(temp, timeptr->tm_zone ? timeptr->tm_zone : "UTC", sizeof(temp)); break;
            case 'z': {
                long off = timeptr->tm_gmtoff;
                char sign = (off < 0) ? '-' : '+';
                if (off < 0) off = -off;
                int off_h = (int)(off / 3600);
                int off_m = (int)((off % 3600) / 60);
                snprintf(temp, sizeof(temp), "%c%02d%02d", sign, off_h, off_m);
                break;
            }
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
