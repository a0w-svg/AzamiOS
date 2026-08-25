/* ============================================================================
 * AzamiOS Userspace — Clock and Timer Tuning (sys/timex.h)
 * File: userland/libc/include/sys/timex.h
 * ============================================================================ */
#pragma once

#include "types.h"
#include "time.h"

struct timex {
    unsigned int modes;
    long         offset;
    long         freq;
    long         maxerror;
    long         esterror;
    int          status;
    long         constant;
    long         precision;
    long         tolerance;
    struct timeval time;
    long         tick;
    long         ppsfreq;
    long         jitter;
    int          shift;
    long         stabil;
    long         jitcnt;
    long         calcnt;
    long         errcnt;
    long         stbcnt;
    int          tai;
};

int adjtimex(struct timex *buf);
int ntp_adjtime(struct timex *buf);
