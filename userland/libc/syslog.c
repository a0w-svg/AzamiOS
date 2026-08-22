/* ============================================================================
 * AzamiOS Userspace — System Logging Implementation (syslog.c)
 * File: userland/libc/syslog.c
 * ============================================================================ */

#include "include/syslog.h"
#include "include/stdio.h"
#include "include/stdlib.h"
#include "include/string.h"
#include "include/unistd.h"
#include "include/time.h"

static char  s_log_ident[64] = "azamios";
static int   s_log_opt = 0;
static int   s_log_fac = LOG_USER;
static int   s_log_mask = 0xFF;
static FILE *s_log_file = NULL;

void openlog(const char *ident, int option, int facility)
{
    if (ident) {
        strncpy(s_log_ident, ident, sizeof(s_log_ident) - 1);
        s_log_ident[sizeof(s_log_ident) - 1] = '\0';
    }
    s_log_opt = option;
    s_log_fac = facility;

    if (!s_log_file) {
        s_log_file = fopen("/var/log/syslog", "a");
    }
}

void closelog(void)
{
    if (s_log_file) {
        fclose(s_log_file);
        s_log_file = NULL;
    }
}

int setlogmask(int mask)
{
    int old = s_log_mask;
    if (mask != 0) s_log_mask = mask;
    return old;
}

void vsyslog(int priority, const char *format, va_list ap)
{
    int pri = priority & LOG_PRIMASK;
    if (!(s_log_mask & LOG_MASK(pri))) return;

    if (!s_log_file) {
        s_log_file = fopen("/var/log/syslog", "a");
    }

    char msg[1024];
    vsnprintf(msg, sizeof(msg), format, ap);

    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char time_str[32];
    if (tm_info) {
        strftime(time_str, sizeof(time_str), "%b %d %H:%M:%S", tm_info);
    } else {
        strcpy(time_str, "Jan 01 00:00:00");
    }

    char line[1200];
    if (s_log_opt & LOG_PID) {
        snprintf(line, sizeof(line), "%s azamios %s[%d]: %s\n", time_str, s_log_ident, getpid(), msg);
    } else {
        snprintf(line, sizeof(line), "%s azamios %s: %s\n", time_str, s_log_ident, msg);
    }

    if (s_log_file) {
        fputs(line, s_log_file);
        fflush(s_log_file);
    }

    if (s_log_opt & LOG_PERROR) {
        fputs(line, stderr);
    }
}

void syslog(int priority, const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    vsyslog(priority, format, ap);
    va_end(ap);
}
