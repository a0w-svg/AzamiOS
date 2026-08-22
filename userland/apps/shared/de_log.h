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

static inline void de_log(const char *msg)
{
    if (!msg) return;
    size_t len = 0;
    while (msg[len]) len++;
    sys_write(1, msg, len);
    sys_write(1, "\n", 1);
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
