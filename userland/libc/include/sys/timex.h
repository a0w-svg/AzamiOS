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

/* Linux adjtimex(2) mode and status bits used by NTP clients. */
#define ADJ_OFFSET   0x0001
#define ADJ_STATUS   0x0010
#define ADJ_MAXERROR 0x0004
#define ADJ_ESTERROR 0x0008
#define STA_PLL      0x0001
#define STA_INS      0x0010
#define STA_DEL      0x0020
#define STA_UNSYNC   0x0040

int adjtimex(struct timex *buf);
int ntp_adjtime(struct timex *buf);
