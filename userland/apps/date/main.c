/* ============================================================================
 * AzamiOS Userspace — System Date & Time (date.elf)
 * File: userland/apps/date/main.c
 *
 * Usage:
 *   date                      Display current date and time in local timezone
 *   date -u, --utc            Display date and time in Coordinated Universal Time (UTC)
 *   date -R, --rfc-2822       Display date and time in RFC 2822 format
 *   date -I, --iso-8601       Display date in ISO 8601 format (YYYY-MM-DD)
 *   date +"FORMAT"            Display custom format using strftime specifiers
 *   date -s, --set "DATE"     Set system RTC date and time (e.g. "2026-08-19 18:30:00")
 *   date -h, --help           Display usage help
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/string.h"
#include "../../libc/include/time.h"
#include "../../libc/include/sys/time.h"
#include "../../libc/include/sys/syscall.h"
#include "../../libc/include/unistd.h"

#define RTC_SET_TIME 0x700A

typedef struct {
    unsigned char second;
    unsigned char minute;
    unsigned char hour;
    unsigned char day;
    unsigned char month;
    unsigned int  year;
} rtc_time_t;

static void print_help(void)
{
    printf("Usage: date [OPTION]... [+FORMAT]\n");
    printf("  or:  date [-u|--utc] [OPTION]...\n");
    printf("  or:  date [-s|--set] \"YYYY-MM-DD HH:MM:SS\"\n\n");
    printf("Display current time in the given FORMAT, or set system time.\n\n");
    printf("Options:\n");
    printf("  -u, --utc, --universal    Print Coordinated Universal Time (UTC)\n");
    printf("  -R, --rfc-2822            Output date and time in RFC 2822 format\n");
    printf("  -I, --iso-8601            Output date in ISO 8601 format (YYYY-MM-DD)\n");
    printf("  -s, --set STRING          Set time described by STRING\n");
    printf("  -h, --help                Display this help message and exit\n\n");
    printf("Format Controls:\n");
    printf("  %%a  Abbreviated weekday name (e.g., Wed)\n");
    printf("  %%A  Full weekday name (e.g., Wednesday)\n");
    printf("  %%b  Abbreviated month name (e.g., Aug)\n");
    printf("  %%B  Full month name (e.g., August)\n");
    printf("  %%d  Day of month (e.g., 01..31)\n");
    printf("  %%e  Day of month, space padded (e.g.,  1..31)\n");
    printf("  %%F  Full date; same as %%Y-%%m-%%d\n");
    printf("  %%H  Hour in 24h format (00..23)\n");
    printf("  %%I  Hour in 12h format (01..12)\n");
    printf("  %%M  Minute (00..59)\n");
    printf("  %%p  Locale's equivalent of either AM or PM\n");
    printf("  %%S  Second (00..60)\n");
    printf("  %%T  Time; same as %%H:%%M:%%S\n");
    printf("  %%Y  Year (e.g., 2026)\n");
    printf("  %%z  Numeric timezone (e.g., +0200)\n");
    printf("  %%Z  Alphabetic timezone abbreviation (e.g., CEST)\n");
}

static int parse_and_set_date(const char *str)
{
    int year = 0, mon = 0, day = 0, hour = 0, min = 0, sec = 0;
    int matches = sscanf(str, "%d-%d-%d %d:%d:%d", &year, &mon, &day, &hour, &min, &sec);
    if (matches < 3) {
        matches = sscanf(str, "%d/%d/%d %d:%d:%d", &year, &mon, &day, &hour, &min, &sec);
    }
    if (matches < 3) {
        fprintf(stderr, "date: invalid date format '%s' (expected YYYY-MM-DD HH:MM:SS)\n", str);
        return 1;
    }

    if (mon < 1 || mon > 12 || day < 1 || day > 31 || hour < 0 || hour > 23 || min < 0 || min > 59 || sec < 0 || sec > 59) {
        fprintf(stderr, "date: date values out of range\n");
        return 1;
    }

    rtc_time_t rt;
    rt.year = (unsigned int)year;
    rt.month = (unsigned char)mon;
    rt.day = (unsigned char)day;
    rt.hour = (unsigned char)hour;
    rt.minute = (unsigned char)min;
    rt.second = (unsigned char)sec;

    int fd = sys_open("/dev/rtc", 0, 0);
    if (fd < 0) {
        fprintf(stderr, "date: cannot open /dev/rtc for setting time\n");
        return 1;
    }

    int ret = (int)syscall3(SYS_ioctl, fd, RTC_SET_TIME, (long)&rt);
    sys_close(fd);

    if (ret != 0) {
        fprintf(stderr, "date: failed to update RTC\n");
        return 1;
    }

    printf("System clock updated to %04d-%02d-%02d %02d:%02d:%02d\n", year, mon, day, hour, min, sec);
    return 0;
}

int main(int argc, char **argv)
{
    tzset();

    int use_utc = 0;
    int rfc_2822 = 0;
    int iso_8601 = 0;
    const char *custom_fmt = NULL;
    const char *set_str = NULL;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (arg[0] == '+') {
            custom_fmt = arg + 1;
        } else if (strcmp(arg, "-u") == 0 || strcmp(arg, "--utc") == 0 || strcmp(arg, "--universal") == 0) {
            use_utc = 1;
        } else if (strcmp(arg, "-R") == 0 || strcmp(arg, "--rfc-2822") == 0) {
            rfc_2822 = 1;
        } else if (strcmp(arg, "-I") == 0 || strcmp(arg, "--iso-8601") == 0) {
            iso_8601 = 1;
        } else if (strcmp(arg, "-s") == 0 || strcmp(arg, "--set") == 0) {
            if (i + 1 < argc) {
                set_str = argv[++i];
            } else {
                fprintf(stderr, "date: option '%s' requires an argument\n", arg);
                return 1;
            }
        } else if (strncmp(arg, "--set=", 6) == 0) {
            set_str = arg + 6;
        } else if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            print_help();
            return 0;
        } else {
            fprintf(stderr, "date: unrecognized option '%s'\nTry 'date --help' for more information.\n", arg);
            return 1;
        }
    }

    if (set_str) {
        return parse_and_set_date(set_str);
    }

    time_t t = time(NULL);
    struct tm tm_info;
    if (use_utc) {
        gmtime_r(&t, &tm_info);
    } else {
        localtime_r(&t, &tm_info);
    }

    char out[128];
    if (custom_fmt) {
        strftime(out, sizeof(out), custom_fmt, &tm_info);
    } else if (rfc_2822) {
        /* RFC 2822 format: e.g. "Wed, 19 Aug 2026 18:30:00 +0200" */
        strftime(out, sizeof(out), "%a, %d %b %Y %T %z", &tm_info);
    } else if (iso_8601) {
        /* ISO 8601 format: e.g. "2026-08-19" */
        strftime(out, sizeof(out), "%F", &tm_info);
    } else {
        /* Default format: e.g. "Wed Aug 19 18:30:00 CEST 2026" */
        strftime(out, sizeof(out), "%a %b %e %T %Z %Y", &tm_info);
    }

    printf("%s\n", out);
    return 0;
}
