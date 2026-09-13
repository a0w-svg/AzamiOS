/* ============================================================================
 * AzamiOS Desktop Environment — Unified Session Logger
 * File: userland/apps/shared/de_log.h
 *
 * Routes DE diagnostic, lifecycle, and component events cleanly to /session.log
 * instead of polluting the serial/terminal standard output.
 * ============================================================================ */
#pragma once

#include "../../libc/include/sys/syscall.h"
#include "../../libc/include/fcntl.h"
#include "../../libc/include/string.h"
#include "../../libc/include/stdio.h"

static inline void de_log(const char *msg)
{
    if (!msg) return;
    char buf[300];
    int n = 0;
    /* Every DE process (azwm, sessiond, taskbar, wallpaper, notifyd, ...)
     * writes to the same fd 1 (the serial console), and each call here used
     * to be two separate sys_write()s (message, then "\n") — with no lock
     * serializing writers, another process's write could land between them,
     * which is exactly the garbled/interleaved boot log this was chasing
     * down. A PID prefix plus a single write() fixes both: one atomic
     * write can't be split by a concurrent writer, and the prefix makes
     * which process said what unambiguous even when two lines do land
     * back-to-back. */
    n += snprintf(buf + n, sizeof(buf) - n, "[pid%d] ", sys_getpid());
    size_t len = 0;
    while (msg[len] && n < (int)sizeof(buf) - 2) { buf[n++] = msg[len++]; }
    buf[n++] = '\n';
    sys_write(1, buf, n);
}

static inline void de_log_fmt(const char *prefix, const char *msg)
{
    if (!prefix && !msg) return;
    if (prefix) {
        size_t plen = 0;
        while (prefix[plen]) plen++;
        sys_write(1, prefix, plen);
    }
    if (msg) {
        size_t mlen = 0;
        while (msg[mlen]) mlen++;
        sys_write(1, msg, mlen);
    }
    sys_write(1, "\n", 1);
}
