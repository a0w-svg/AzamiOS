/* ============================================================================
 * AzamiOS Userspace — Sleep Utility (sleep.elf)
 * File: userland/apps/sleep/main.c
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/string.h"
#include "../../libc/include/time.h"
#include "../../libc/include/unistd.h"

static void print_help(void)
{
    printf("Usage: sleep NUMBER[SUFFIX]...\n"
           "  or:  sleep OPTION\n"
           "Pause for NUMBER seconds. SUFFIX may be 's' for seconds (default),\n"
           "'m' for minutes, 'h' for hours or 'd' for days. NUMBER may be an arbitrary\n"
           "floating point number. Given two or more arguments, pause for the amount of\n"
           "time specified by the sum of their values.\n\n"
           "      --help     display this help and exit\n"
           "      --version  output version information and exit\n");
}

static void print_version(void)
{
    printf("sleep (AzamiOS coreutils) 7.0.0\n");
}

static int parse_duration(const char *arg, double *out_seconds)
{
    char *endptr = NULL;
    double val = strtod(arg, &endptr);
    if (endptr == arg) {
        return -1;
    }

    double multiplier = 1.0;
    if (*endptr != '\0') {
        switch (*endptr) {
        case 's': multiplier = 1.0; break;
        case 'm': multiplier = 60.0; break;
        case 'h': multiplier = 3600.0; break;
        case 'd': multiplier = 86400.0; break;
        default:
            return -1;
        }
        if (*(endptr + 1) != '\0') {
            return -1;
        }
    }

    if (val < 0.0) return -1;
    *out_seconds = val * multiplier;
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "sleep: missing operand\nTry 'sleep --help' for more information.\n");
        return 1;
    }

    if (argc == 2) {
        if (strcmp(argv[1], "--help") == 0) {
            print_help();
            return 0;
        }
        if (strcmp(argv[1], "--version") == 0) {
            print_version();
            return 0;
        }
    }

    double total_seconds = 0.0;
    for (int i = 1; i < argc; i++) {
        double s = 0.0;
        if (parse_duration(argv[i], &s) < 0) {
            fprintf(stderr, "sleep: invalid time interval '%s'\nTry 'sleep --help' for more information.\n", argv[i]);
            return 1;
        }
        total_seconds += s;
    }

    if (total_seconds <= 0.0) {
        return 0;
    }

    time_t sec = (time_t)total_seconds;
    long nsec = (long)((total_seconds - (double)sec) * 1e9);
    if (nsec >= 1000000000L) {
        sec++;
        nsec -= 1000000000L;
    }

    struct timespec ts;
    ts.tv_sec = sec;
    ts.tv_nsec = nsec;

    nanosleep(&ts, NULL);
    return 0;
}
