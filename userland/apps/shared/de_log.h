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
    int fd = sys_open("/session.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        size_t len = 0;
        while (msg[len]) len++;
        sys_write(fd, msg, len);
        sys_write(fd, "\n", 1);
        sys_close(fd);
    }
}

static inline void de_log_fmt(const char *prefix, const char *msg)
{
    if (!prefix && !msg) return;
    int fd = sys_open("/session.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        if (prefix) {
            size_t plen = 0;
            while (prefix[plen]) plen++;
            sys_write(fd, prefix, plen);
        }
        if (msg) {
            size_t mlen = 0;
            while (msg[mlen]) mlen++;
            sys_write(fd, msg, mlen);
        }
        sys_write(fd, "\n", 1);
        sys_close(fd);
    }
}
