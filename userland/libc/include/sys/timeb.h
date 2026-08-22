/* ============================================================================
 * AzamiOS Userspace — Time Buffer Header (sys/timeb.h)
 * File: userland/libc/include/sys/timeb.h
 * ============================================================================ */
#pragma once

#include "../time.h"

struct timeb {
    time_t         time;
    unsigned short millitm;
    short          timezone;
    short          dstflag;
};

int ftime(struct timeb *tp);
