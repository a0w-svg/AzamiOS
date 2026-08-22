/* ============================================================================
 * AzamiOS Userspace — Access and Modification Times (utime.h)
 * File: userland/libc/include/utime.h
 * ============================================================================ */
#pragma once

#include "sys/types.h"

struct utimbuf {
    time_t actime;  /* Access time */
    time_t modtime; /* Modification time */
};

int utime(const char *filename, const struct utimbuf *times);
