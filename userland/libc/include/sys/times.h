/* ============================================================================
 * AzamiOS Userspace — Process Times Header (sys/times.h)
 * File: userland/libc/include/sys/times.h
 * ============================================================================ */
#pragma once

#include "types.h"

struct tms {
    clock_t tms_utime;  /* User CPU time */
    clock_t tms_stime;  /* System CPU time */
    clock_t tms_cutime; /* User CPU time of dead children */
    clock_t tms_cstime; /* System CPU time of dead children */
};

clock_t times(struct tms *buf);
