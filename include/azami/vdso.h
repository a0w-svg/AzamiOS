/* ============================================================================
 * AzamiOS — vDSO / vvar shared data layout
 * File: include/azami/vdso.h
 *
 * The one structure the kernel's timekeeper (kernel/time/timekeeping.c) and
 * the user-mode vDSO (arch/x86_64/vdso/vclock.c) both see. It lives in a
 * single physical page ("vvar") that the kernel writes through the HHDM and
 * every process maps read-only two pages below its vDSO text:
 *
 *     vdso_base - 2*PAGE   [vvar]  struct vdso_data          (r--)
 *     vdso_base - 1*PAGE   [vvar]  HPET MMIO page, UC        (r--, HPET mode only)
 *     vdso_base            [vdso]  ELF image (linux-vdso.so.1) (r-x)
 *
 * Consistency is a sequence count, exactly like Linux's vdso_data::seq: the
 * writer makes it odd, updates, makes it even; a reader retries if it saw an
 * odd value or the value changed underneath it.
 *
 * Time representation (also Linux's): for each hi-res clock the kernel
 * publishes, as of the clocksource reading `cycle_last`,
 *
 *     basetime[clk].sec                     whole seconds
 *     basetime[clk].nsec                    nanoseconds << shift
 *
 * and a reader computes
 *
 *     ns  = basetime.nsec + ((now - cycle_last) & mask) * mult;
 *     ns >>= shift;       sec = basetime.sec + ns / NSEC_PER_SEC ...
 *
 * Coarse clocks (REALTIME_COARSE / MONOTONIC_COARSE) store unshifted nsec and
 * are returned without touching the clocksource at all.
 *
 * Freestanding: no kernel headers, so the vDSO can include it as-is.
 * ============================================================================ */
#pragma once

#define VDSO_CLOCKMODE_NONE     0   /* no user-readable counter: use syscall */
#define VDSO_CLOCKMODE_TSC      1   /* RDTSC                                  */
#define VDSO_CLOCKMODE_HPET     2   /* HPET main counter via the mapped page  */

#define VDSO_GETCPU_SYSCALL     0
#define VDSO_GETCPU_RDPID       1   /* RDPID reads IA32_TSC_AUX directly      */
#define VDSO_GETCPU_RDTSCP      2   /* RDTSCP returns IA32_TSC_AUX in ECX     */

/* Linux clockid_t values. */
#define VDSO_CLOCK_REALTIME             0
#define VDSO_CLOCK_MONOTONIC            1
#define VDSO_CLOCK_PROCESS_CPUTIME_ID   2
#define VDSO_CLOCK_THREAD_CPUTIME_ID    3
#define VDSO_CLOCK_MONOTONIC_RAW        4
#define VDSO_CLOCK_REALTIME_COARSE      5
#define VDSO_CLOCK_MONOTONIC_COARSE     6
#define VDSO_CLOCK_BOOTTIME             7
#define VDSO_CLOCK_REALTIME_ALARM       8
#define VDSO_CLOCK_BOOTTIME_ALARM       9
#define VDSO_CLOCK_TAI                  11
#define VDSO_NR_CLOCKS                  12

/* Bit n set = clock n is served from basetime[] with the hi-res formula. */
#define VDSO_HRES_MASK   ((1u << VDSO_CLOCK_REALTIME) | (1u << VDSO_CLOCK_MONOTONIC) | \
                          (1u << VDSO_CLOCK_BOOTTIME) | (1u << VDSO_CLOCK_TAI))
#define VDSO_COARSE_MASK ((1u << VDSO_CLOCK_REALTIME_COARSE) | \
                          (1u << VDSO_CLOCK_MONOTONIC_COARSE))
/* MONOTONIC_RAW is hi-res too but uses raw_mult (never frequency-adjusted). */
#define VDSO_RAW_MASK    (1u << VDSO_CLOCK_MONOTONIC_RAW)

struct vdso_timestamp {
    unsigned long long sec;
    unsigned long long nsec;
};

struct vdso_data {
    volatile unsigned int   seq;
    int                     clock_mode;     /* VDSO_CLOCKMODE_*                 */
    unsigned long long      cycle_last;     /* counter value basetime refers to */
    unsigned long long      mask;           /* counter width mask               */
    unsigned int            mult;           /* adjusted (NTP/adjtime) mult      */
    unsigned int            shift;
    unsigned int            raw_mult;       /* unadjusted mult, MONOTONIC_RAW   */
    unsigned int            hrtimer_res;    /* ns reported by clock_getres()    */
    struct vdso_timestamp   basetime[VDSO_NR_CLOCKS];

    int                     tz_minuteswest; /* settimeofday(2)'s struct timezone */
    int                     tz_dsttime;

    int                     getcpu_mode;    /* VDSO_GETCPU_*                    */
    unsigned int            hpet_offset;    /* main counter offset in HPET page */
    unsigned int            coarse_res;     /* ns: one scheduler tick           */
    unsigned int            _pad;
};

#define VDSO_NSEC_PER_SEC 1000000000ULL
