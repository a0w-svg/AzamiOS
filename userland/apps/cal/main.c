/* ============================================================================
 * AzamiOS Userspace — Monthly Calendar Utility (cal.elf)
 * File: userland/apps/cal/main.c
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/time.h"

static const char *g_month_names[] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"
};

static const int g_days_in_month[] = {
    31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
};

static int is_leap(int year)
{
    return ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0));
}

/* Day of week for 1st of month: 0=Sun, 1=Mon, ..., 6=Sat */
static int day_of_week(int d, int m, int y)
{
    static int t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (m < 3) y -= 1;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

int main(int argc, char **argv)
{
    int month = 8;
    int year = 2026;

    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    if (tm_info) {
        month = tm_info->tm_mon + 1;
        year = tm_info->tm_year + 1900;
    }

    if (argc == 2) {
        year = atoi(argv[1]);
        if (year < 1 || year > 9999) year = 2026;
    } else if (argc >= 3) {
        month = atoi(argv[1]);
        year = atoi(argv[2]);
        if (month < 1 || month > 12) month = 1;
        if (year < 1 || year > 9999) year = 2026;
    }

    int days = g_days_in_month[month - 1];
    if (month == 2 && is_leap(year)) days = 29;

    int first_dow = day_of_week(1, month, year);

    printf("    %s %d\n", g_month_names[month - 1], year);
    printf("Su Mo Tu We Th Fr Sa\n");

    /* Print leading spaces */
    for (int i = 0; i < first_dow; i++) {
        printf("   ");
    }

    int today_day = (tm_info) ? tm_info->tm_mday : -1;
    int today_mon = (tm_info) ? (tm_info->tm_mon + 1) : -1;
    int today_yr  = (tm_info) ? (tm_info->tm_year + 1900) : -1;

    for (int d = 1; d <= days; d++) {
        if (d == today_day && month == today_mon && year == today_yr) {
            printf("\033[1;35;7m%2d\033[0m ", d);
        } else {
            printf("%2d ", d);
        }
        if ((first_dow + d) % 7 == 0 || d == days) {
            printf("\n");
        }
    }

    return 0;
}
