/* ============================================================================
 * AzamiOS — Timekeeping core (clocksources, POSIX clocks, NTP discipline)
 * File: kernel/time/timekeeping.h
 *
 * One timekeeper serves every clock the kernel and user space read:
 *
 *   - A clocksource (TSC, HPET or the scheduler tick) is converted to
 *     nanoseconds with a mult/shift pair and accumulated into CLOCK_MONOTONIC
 *     and CLOCK_MONOTONIC_RAW on every BSP tick.
 *   - CLOCK_REALTIME is CLOCK_MONOTONIC plus an offset, seeded from the CMOS
 *     RTC at boot and moved by clock_settime()/settimeofday()/adjtimex().
 *   - The result is published, under a sequence count, into the vvar page
 *     (include/azami/vdso.h) that the vDSO reads from user space.
 *
 * adjtimex(2) is a real (simplified) NTP discipline: ADJ_FREQUENCY and
 * ADJ_TICK steer the clock rate, ADJ_OFFSET / ADJ_OFFSET_SINGLESHOT slew
 * the offset away at up to 500 ppm, ADJ_SETOFFSET steps it, and STA_INS /
 * STA_DEL insert or delete a leap second at the next UTC midnight.
 * ============================================================================ */
#pragma once

#include "../../include/azami/types.h"

/* Linux struct timex (uapi/linux/timex.h), byte-for-byte. */
struct kernel_timex {
    unsigned int modes;
    int          _pad0;
    long         offset;
    long         freq;
    long         maxerror;
    long         esterror;
    int          status;
    int          _pad1;
    long         constant;
    long         precision;
    long         tolerance;
    long         time_sec;      /* struct timeval / timespec (STA_NANO) */
    long         time_usec;
    long         tick;
    long         ppsfreq;
    long         jitter;
    int          shift;
    int          _pad2;
    long         stabil;
    long         jitcnt;
    long         calcnt;
    long         errcnt;
    long         stbcnt;
    int          tai;
    int          _reserved[11];
};

/* adjtimex modes */
#define ADJ_OFFSET              0x0001
#define ADJ_FREQUENCY           0x0002
#define ADJ_MAXERROR            0x0004
#define ADJ_ESTERROR            0x0008
#define ADJ_STATUS              0x0010
#define ADJ_TIMECONST           0x0020
#define ADJ_TAI                 0x0080
#define ADJ_SETOFFSET           0x0100
#define ADJ_MICRO               0x1000
#define ADJ_NANO                0x2000
#define ADJ_TICK                0x4000
#define ADJ_OFFSET_SINGLESHOT   0x8001
#define ADJ_OFFSET_SS_READ      0xa001

/* status bits */
#define STA_PLL         0x0001
#define STA_PPSFREQ     0x0002
#define STA_PPSTIME     0x0004
#define STA_FLL         0x0008
#define STA_INS         0x0010
#define STA_DEL         0x0020
#define STA_UNSYNC      0x0040
#define STA_FREQHOLD    0x0080
#define STA_PPSSIGNAL   0x0100
#define STA_PPSJITTER   0x0200
#define STA_PPSWANDER   0x0400
#define STA_PPSERROR    0x0800
#define STA_CLOCKERR    0x1000
#define STA_NANO        0x2000
#define STA_MODE        0x4000
#define STA_CLK         0x8000
#define STA_RONLY       (STA_PPSSIGNAL | STA_PPSJITTER | STA_PPSWANDER | \
                         STA_PPSERROR | STA_CLOCKERR | STA_NANO | STA_MODE | STA_CLK)

/* clock states returned by adjtimex */
#define TIME_OK     0
#define TIME_INS    1
#define TIME_DEL    2
#define TIME_OOP    3
#define TIME_WAIT   4
#define TIME_ERROR  5

#define NSEC_PER_SEC    1000000000ULL
#define NSEC_PER_USEC   1000ULL
#define TK_HZ           100             /* the scheduler tick rate */

/** timekeeping_init() — pick the best clocksource, seed REALTIME from the RTC
 *  and start publishing to the vvar page. After smp_init() (the TSC is
 *  calibrated there), before anything reads the time. */
void timekeeping_init(void);

/** timekeeping_tick() — accumulate the clocksource; called from the BSP's
 *  timer interrupt, once per tick. */
void timekeeping_tick(void);

/** Nanoseconds on CLOCK_MONOTONIC / MONOTONIC_RAW / REALTIME. */
u64 ktime_get_ns(void);
u64 ktime_get_raw_ns(void);
u64 ktime_get_real_ns(void);

/** Seconds on CLOCK_REALTIME, as of the last tick (no counter read). */
u64 ktime_get_real_seconds(void);

/** ktime_get_clock(clk, sec, nsec) — read any clock the timekeeper owns
 *  (REALTIME, MONOTONIC, MONOTONIC_RAW, the COARSE pair, BOOTTIME, TAI and
 *  the two ALARM clocks). Returns 0 or -EINVAL for any other id. */
int ktime_get_clock(u32 clk, s64 *sec, s64 *nsec);

/** timekeeping_clock_res_ns(clk) — resolution for clock_getres(), 0 if the
 *  timekeeper does not own @clk. */
u32 timekeeping_clock_res_ns(u32 clk);

/** timekeeping_settime(sec, nsec) — step CLOCK_REALTIME. -EINVAL if out of
 *  range. Monotonic clocks are unaffected, as on Linux. */
int timekeeping_settime(s64 sec, s64 nsec);

/** timekeeping_set_tz() — record settimeofday(2)'s struct timezone. */
void timekeeping_set_tz(int minuteswest, int dsttime);
void timekeeping_get_tz(int *minuteswest, int *dsttime);

/** timekeeping_adjtimex(tx, may_set) — the body of adjtimex(2) and
 *  clock_adjtime(CLOCK_REALTIME). @may_set is the caller's CAP_SYS_TIME.
 *  Returns the TIME_* clock state, or a negative errno. */
int timekeeping_adjtimex(struct kernel_timex *tx, bool may_set);

/** Clocksource introspection / selection (sysfs clocksource0). */
const char *timekeeping_current_clocksource(void);
size_t      timekeeping_available_clocksources(char *buf, size_t len);
int         timekeeping_select_clocksource(const char *name);

/** The vvar page the vDSO maps, and its HPET companion page (0 if none). */
phys_addr_t timekeeping_vvar_phys(void);
phys_addr_t timekeeping_hpet_page_phys(void);
