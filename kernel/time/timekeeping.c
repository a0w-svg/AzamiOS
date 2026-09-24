/* ============================================================================
 * AzamiOS — Timekeeping core
 * File: kernel/time/timekeeping.c
 *
 * See timekeeping.h for the model. The implementation follows Linux's
 * kernel/time/timekeeping.c closely where it matters for correctness:
 *
 *   - Nanoseconds are kept left-shifted ("snsec") so the per-cycle rate can
 *     be fractional: ns = (snsec + delta * mult) >> shift.
 *   - mult/shift are chosen by clocks_calc_mult_shift() so that ten minutes
 *     of counter delta can be multiplied without 64-bit overflow — a reader
 *     between two ticks never gets anywhere near that.
 *   - The rate the kernel steers (NTP frequency, adjtime slew) only ever
 *     changes `mult` at an accumulation point, so time stays continuous.
 *   - Every update is published to the vvar page under a sequence count;
 *     in-kernel readers use the very same page and algorithm as the vDSO,
 *     so the kernel and user space can never disagree about the time.
 *
 * Locking: g_tk_lock (IRQ-safe spinlock) serialises writers — the BSP tick,
 * clock_settime(), adjtimex() and clocksource switches. Readers never lock.
 * ============================================================================ */

#include "timekeeping.h"
#include "../../include/azami/defs.h"
#include "../../include/azami/vdso.h"
#include "../../arch/x86_64/cpu/cpu.h"
#include "../../arch/x86_64/cpu/msr.h"
#include "../../arch/x86_64/cpu/lapic.h"
#include "../../arch/x86_64/cpu/spinlock.h"
#include "../../drivers/misc/hpet.h"
#include "../../drivers/misc/rtc.h"
#include "../../drivers/char/console.h"
#include "../mm/pmm.h"
#include "../lib/string.h"
#include "../sched/sched.h"

/* ── Clocksources ─────────────────────────────────────────────────────────── */

typedef struct clocksource {
    const char *name;
    u64       (*read)(void);
    u64         mask;
    u64         freq_hz;
    int         vdso_mode;      /* VDSO_CLOCKMODE_* */
    int         rating;         /* higher is better */
    bool        usable;
} clocksource_t;

static u64 cs_tsc_read(void)     { return rdtsc_ordered(); }
static u64 cs_hpet_read(void)    { return hpet_read_counter(); }
static u64 cs_jiffies_read(void) { return sched_get_ticks(); }

static clocksource_t g_cs_tsc     = { "tsc",     cs_tsc_read,     ~0ULL, 0,     VDSO_CLOCKMODE_TSC,  300, false };
static clocksource_t g_cs_hpet    = { "hpet",    cs_hpet_read,    ~0ULL, 0,     VDSO_CLOCKMODE_HPET, 250, false };
static clocksource_t g_cs_jiffies = { "jiffies", cs_jiffies_read, ~0ULL, TK_HZ, VDSO_CLOCKMODE_NONE, 1,   true  };

static clocksource_t *const g_clocksources[] = { &g_cs_tsc, &g_cs_hpet, &g_cs_jiffies };
#define NR_CLOCKSOURCES (sizeof(g_clocksources) / sizeof(g_clocksources[0]))

/* ── Timekeeper state ─────────────────────────────────────────────────────── */

#define TK_MAXSEC           600     /* longest delta mult/shift must survive  */
#define MAXFREQ_SCALED      (500LL << 16)   /* ±500 ppm, in ppm << 16         */
#define MAX_SLEW_PPM        500             /* adjtime(3) slew rate           */
#define NTP_PHASE_LIMIT     16000000L       /* maxerror cap, usec             */
#define SECS_PER_DAY        86400

static spinlock_t          g_tk_lock = SPINLOCK_INIT;
static struct vdso_data   *g_vd;            /* HHDM alias of the vvar page    */
static phys_addr_t         g_vvar_phys;
static phys_addr_t         g_hpet_page_phys;
static volatile bool       g_tk_ready;

static struct timekeeper {
    clocksource_t *cs;
    u64  cycle_last;
    u64  mask;
    u64  max_cycles;        /* largest delta multiplied in one step        */
    u32  shift;
    u32  base_mult;         /* nominal ns-per-cycle << shift                */
    u32  raw_mult;          /* == base_mult: MONOTONIC_RAW is never steered */
    u32  mult;              /* base_mult steered by freq, tick and slew     */

    u64  mono_sec,  mono_snsec;
    u64  raw_sec,   raw_snsec;

    /* CLOCK_REALTIME = CLOCK_MONOTONIC + (real_off_sec, real_off_nsec),
     * with real_off_nsec normalised into [0, NSEC_PER_SEC). */
    s64  real_off_sec;
    u64  real_off_nsec;
    s32  tai_off;           /* TAI - UTC, seconds                           */
    u64  last_real_sec;     /* second_overflow() bookkeeping                */

    /* NTP discipline */
    int  status;
    int  state;
    long freq;              /* ppm << 16                                    */
    long tick_usec;         /* ADJ_TICK: usec per nominal 10 ms tick        */
    long maxerror;
    long esterror;
    long constant;
    s64  slew_ns;           /* offset still to be slewed into REALTIME      */
    bool slew_singleshot;   /* slew came from adjtime(3), reported in usec  */
    s64  slew_mult_delta;   /* signed extra mult while slewing              */
    u64  slew_frac;         /* sub-ns remainder of applied slew, << shift   */

    /* TSC frequency refinement against the HPET */
    bool refine_active;
    int  refine_stage;
    u64  refine_tsc0;
    u64  refine_hpet_last;
    u64  refine_hpet_cycles;

    int  tz_minuteswest;
    int  tz_dsttime;
} tk;

/* ── Arithmetic helpers ───────────────────────────────────────────────────── */

/* a * b / c with a 128-bit intermediate. The quotient must fit in 64 bits
 * (every caller's does); a freestanding kernel has no __udivti3. */
static inline u64 mul_u64_u64_div_u64(u64 a, u64 b, u64 c)
{
    u64 q, r;
    __asm__("mulq %3\n\t"
            "divq %4"
            : "=a"(q), "=&d"(r)
            : "0"(a), "r"(b), "r"(c)
            : "cc");
    (void)r;
    return q;
}

/* Linux's clocks_calc_mult_shift(): the largest shift whose mult still
 * converts @maxsec seconds of @from-Hz counter without overflowing 64 bits. */
static void calc_mult_shift(u32 *mult, u32 *shift, u64 from, u64 to, u32 maxsec)
{
    u64 tmp = ((u64)maxsec * from) >> 32;
    u32 sftacc = 32;
    while (tmp) {
        tmp >>= 1;
        sftacc--;
    }
    u32 sft;
    for (sft = 32; sft > 0; sft--) {
        tmp = (to << sft) + from / 2;
        tmp /= from;
        if ((tmp >> sftacc) == 0) break;
    }
    *mult  = (u32)tmp;
    *shift = sft;
}

static inline void normalize(u64 *sec, u64 *snsec, u32 shift)
{
    const u64 lim = NSEC_PER_SEC << shift;
    while (*snsec >= lim) {
        *snsec -= lim;
        (*sec)++;
    }
}

/* ── Rate steering ────────────────────────────────────────────────────────── */

/* Recompute tk.mult from the NTP frequency, ADJ_TICK and any active slew.
 * Caller holds g_tk_lock and has just accumulated. */
static void tk_update_mult(void)
{
    /* Total steering in ppm << 16. ADJ_TICK expresses a rate too: a tick of
     * 10100 usec means the clock should advance 1% faster. */
    s64 ppm_scaled = tk.freq;
    ppm_scaled += (s64)(tk.tick_usec - 1000000 / TK_HZ) * 100 * 65536;

    s64 m = (s64)tk.base_mult + ((s64)tk.base_mult * ppm_scaled) / (1000000LL * 65536);
    tk.slew_mult_delta = 0;
    if (tk.slew_ns) {
        s64 d = ((s64)tk.base_mult * MAX_SLEW_PPM) / 1000000;
        if (d == 0) d = 1;
        tk.slew_mult_delta = tk.slew_ns > 0 ? d : -d;
        m += tk.slew_mult_delta;
    }
    if (m < 1) m = 1;
    if (m > 0xFFFFFFFFLL) m = 0xFFFFFFFFLL;
    tk.mult = (u32)m;
}

/* Recompute everything that derives from the clocksource frequency.
 * Keeps the current shift when @keep_shift (frequency refinement), so the
 * accumulated snsec values stay valid. */
static void tk_setup_rate(bool keep_shift)
{
    u32 mult, shift;
    calc_mult_shift(&mult, &shift, tk.cs->freq_hz, NSEC_PER_SEC, TK_MAXSEC);
    if (keep_shift && shift != tk.shift) {
        /* Same shift, fresh mult: (1e9 << shift) / freq, rounded. */
        mult = (u32)(((NSEC_PER_SEC << tk.shift) + tk.cs->freq_hz / 2) / tk.cs->freq_hz);
        shift = tk.shift;
    }
    tk.shift     = shift;
    tk.base_mult = mult;
    tk.raw_mult  = mult;
    tk.mask      = tk.cs->mask;

    /* Keep delta * mult (with the steering headroom) below 2^63. */
    u64 mmax = (u64)mult + (u64)mult / 64 + 1;
    tk.max_cycles = (~0ULL >> 1) / mmax;
    if (tk.max_cycles > tk.mask) tk.max_cycles = tk.mask;
    tk_update_mult();
}

/* ── Accumulation ─────────────────────────────────────────────────────────── */

static void tk_apply_slew(u64 cycles)
{
    if (!tk.slew_ns || !tk.slew_mult_delta) return;
    u64 per = (u64)(tk.slew_mult_delta < 0 ? -tk.slew_mult_delta : tk.slew_mult_delta);
    u64 sn  = cycles * per + tk.slew_frac;
    u64 ns  = sn >> tk.shift;
    tk.slew_frac = sn & ((1ULL << tk.shift) - 1);

    u64 left = (u64)(tk.slew_ns < 0 ? -tk.slew_ns : tk.slew_ns);
    if (ns < left) {
        tk.slew_ns += tk.slew_ns > 0 ? -(s64)ns : (s64)ns;
        return;
    }
    /* Done: the last step overshot by (ns - left). Take the overshoot back
     * out of the REALTIME offset directly — at 500 ppm over one tick it is
     * at most a few microseconds. */
    s64 over = (s64)(ns - left);
    s64 corr = tk.slew_ns > 0 ? -over : over;
    s64 n = (s64)tk.real_off_nsec + corr;
    while (n < 0)                    { n += (s64)NSEC_PER_SEC; tk.real_off_sec--; }
    while (n >= (s64)NSEC_PER_SEC)   { n -= (s64)NSEC_PER_SEC; tk.real_off_sec++; }
    tk.real_off_nsec = (u64)n;
    tk.slew_ns   = 0;
    tk.slew_frac = 0;
    tk.slew_singleshot = false;
    tk_update_mult();
}

static void tk_accumulate(u64 now)
{
    u64 delta = (now - tk.cycle_last) & tk.mask;
    if (tk.cs == &g_cs_tsc && (s64)(now - tk.cycle_last) < 0) return;
    tk.cycle_last = now;

    while (delta) {
        u64 d = delta > tk.max_cycles ? tk.max_cycles : delta;
        delta -= d;
        tk.mono_snsec += d * tk.mult;
        tk.raw_snsec  += d * tk.raw_mult;
        normalize(&tk.mono_sec, &tk.mono_snsec, tk.shift);
        normalize(&tk.raw_sec,  &tk.raw_snsec,  tk.shift);
        tk_apply_slew(d);
    }
}

static void tk_real(u64 *sec, u64 *snsec)
{
    *sec   = tk.mono_sec + (u64)tk.real_off_sec;
    *snsec = tk.mono_snsec + (tk.real_off_nsec << tk.shift);
    normalize(sec, snsec, tk.shift);
}

/* Linux's second_overflow(): leap seconds and the maxerror drift estimate,
 * run once for every REALTIME second that passes. */
static void tk_second_overflow(u64 secs)
{
    switch (tk.state) {
    case TIME_OK:
        if (tk.status & STA_INS)      tk.state = TIME_INS;
        else if (tk.status & STA_DEL) tk.state = TIME_DEL;
        break;
    case TIME_INS:
        if (!(tk.status & STA_INS)) {
            tk.state = TIME_OK;
        } else if (secs % SECS_PER_DAY == 0) {
            /* 23:59:60 — replay the last second of the day. */
            tk.real_off_sec--;
            tk.tai_off++;
            tk.state = TIME_OOP;
            kprintf("[TIME] Clock: inserting leap second 23:59:60 UTC\n");
        }
        break;
    case TIME_DEL:
        if (!(tk.status & STA_DEL)) {
            tk.state = TIME_OK;
        } else if ((secs + 1) % SECS_PER_DAY == 0) {
            tk.real_off_sec++;
            tk.tai_off--;
            tk.state = TIME_WAIT;
            kprintf("[TIME] Clock: deleting leap second 23:59:59 UTC\n");
        }
        break;
    case TIME_OOP:
        tk.state = TIME_WAIT;
        break;
    case TIME_WAIT:
        if (!(tk.status & (STA_INS | STA_DEL))) tk.state = TIME_OK;
        break;
    }

    tk.maxerror += 500;     /* MAXFREQ / NSEC_PER_USEC, as Linux */
    if (tk.maxerror > NTP_PHASE_LIMIT) {
        tk.maxerror = NTP_PHASE_LIMIT;
        tk.status |= STA_UNSYNC;
    }
}

/* ── Publication ──────────────────────────────────────────────────────────── */

static inline void tk_seq_begin(void)
{
    g_vd->seq++;
    __asm__ volatile("" ::: "memory");  /* x86 TSO: stores stay ordered */
}

static inline void tk_seq_end(void)
{
    __asm__ volatile("" ::: "memory");
    g_vd->seq++;
}

static void tk_publish(void)
{
    struct vdso_data *vd = g_vd;
    vd->clock_mode = tk.cs->vdso_mode;
    vd->cycle_last = tk.cycle_last;
    vd->mask       = tk.mask;
    vd->mult       = tk.mult;
    vd->raw_mult   = tk.raw_mult;
    vd->shift      = tk.shift;

    u64 hres = NSEC_PER_SEC / tk.cs->freq_hz;
    vd->hrtimer_res = (u32)(hres ? hres : 1);
    vd->coarse_res  = (u32)(NSEC_PER_SEC / TK_HZ);

    vd->basetime[VDSO_CLOCK_MONOTONIC].sec      = tk.mono_sec;
    vd->basetime[VDSO_CLOCK_MONOTONIC].nsec     = tk.mono_snsec;
    vd->basetime[VDSO_CLOCK_BOOTTIME]           = vd->basetime[VDSO_CLOCK_MONOTONIC];
    vd->basetime[VDSO_CLOCK_MONOTONIC_RAW].sec  = tk.raw_sec;
    vd->basetime[VDSO_CLOCK_MONOTONIC_RAW].nsec = tk.raw_snsec;

    u64 rs, rsn;
    tk_real(&rs, &rsn);
    vd->basetime[VDSO_CLOCK_REALTIME].sec  = rs;
    vd->basetime[VDSO_CLOCK_REALTIME].nsec = rsn;
    vd->basetime[VDSO_CLOCK_TAI].sec       = rs + (u64)(s64)tk.tai_off;
    vd->basetime[VDSO_CLOCK_TAI].nsec      = rsn;

    vd->basetime[VDSO_CLOCK_REALTIME_COARSE].sec   = rs;
    vd->basetime[VDSO_CLOCK_REALTIME_COARSE].nsec  = rsn >> tk.shift;
    vd->basetime[VDSO_CLOCK_MONOTONIC_COARSE].sec  = tk.mono_sec;
    vd->basetime[VDSO_CLOCK_MONOTONIC_COARSE].nsec = tk.mono_snsec >> tk.shift;

    vd->tz_minuteswest = tk.tz_minuteswest;
    vd->tz_dsttime     = tk.tz_dsttime;
}

/* ── TSC refinement ───────────────────────────────────────────────────────── */

/* The boot-time TSC calibration (lapic_timer_calibrate) measures a 10 ms
 * window; its error, a few hundred ppm, is a few seconds a day of drift in
 * CLOCK_REALTIME. Re-measure against the HPET over 1 s, 10 s and 100 s and
 * adopt each better estimate, as Linux's tsc_refine_calibration_work does. */
static const u64 g_refine_windows_sec[] = { 1, 10, 100 };

static void tk_refine_start(void)
{
    tk.refine_active = (tk.cs == &g_cs_tsc) && hpet_available();
    if (!tk.refine_active) return;
    tk.refine_stage       = 0;
    tk.refine_tsc0        = rdtsc_ordered();
    tk.refine_hpet_last   = hpet_read_counter();
    tk.refine_hpet_cycles = 0;
}

static void tk_refine_tick(void)
{
    if (!tk.refine_active) return;
    u64 h = hpet_read_counter();
    u64 t = rdtsc_ordered();
    tk.refine_hpet_cycles += (h - tk.refine_hpet_last) & hpet_counter_mask();
    tk.refine_hpet_last = h;

    u64 hfreq = hpet_frequency_hz();
    if (tk.refine_hpet_cycles < g_refine_windows_sec[tk.refine_stage] * hfreq) return;

    u64 tsc_delta = t - tk.refine_tsc0;
    u64 freq = mul_u64_u64_div_u64(tsc_delta, hfreq, tk.refine_hpet_cycles);
    u64 old  = tk.cs->freq_hz;
    u64 diff = freq > old ? freq - old : old - freq;

    if (freq && diff < old / 100) {     /* sanity: within 1% */
        tk.cs->freq_hz = freq;
        tk_setup_rate(true);
        kprintf("[TIME] TSC refined over %llus: %llu.%06llu MHz (%lld ppm)\n",
                (unsigned long long)g_refine_windows_sec[tk.refine_stage],
                (unsigned long long)(freq / 1000000),
                (unsigned long long)(freq % 1000000),
                (long long)(((s64)freq - (s64)old) * 1000000 / (s64)old));
    } else {
        kprintf("[TIME] TSC refinement rejected (%llu Hz vs %llu Hz)\n",
                (unsigned long long)freq, (unsigned long long)old);
    }

    if (++tk.refine_stage >= (int)(sizeof(g_refine_windows_sec) / sizeof(g_refine_windows_sec[0])))
        tk.refine_active = false;
}

/* ── Tick ─────────────────────────────────────────────────────────────────── */

void timekeeping_tick(void)
{
    if (!g_tk_ready) return;
    irqflags_t f = spinlock_lock_irqsave(&g_tk_lock);

    tk_seq_begin();
    tk_accumulate(tk.cs->read());
    tk_refine_tick();

    u64 rs, rsn;
    tk_real(&rs, &rsn);
    /* Bounded catch-up: a stall of minutes needs no per-second replay. */
    if (rs > tk.last_real_sec + 2) tk.last_real_sec = rs - 2;
    while (tk.last_real_sec < rs) {
        tk.last_real_sec++;
        tk_second_overflow(tk.last_real_sec);
    }

    tk_publish();
    tk_seq_end();

    spinlock_unlock_irqrestore(&g_tk_lock, f);
}

/* ── Readers ──────────────────────────────────────────────────────────────── */

/* Before timekeeping_init() the only time is the tick (or the HPET). */
static u64 early_mono_ns(void)
{
    if (hpet_available()) return hpet_now_ns();
    return sched_get_ticks() * (NSEC_PER_SEC / TK_HZ);
}

static void tk_read_hres(u32 clk, u64 *sec_out, u64 *ns_out)
{
    const struct vdso_data *vd = g_vd;
    u64 sec, ns;
    u32 seq;
    do {
        while ((seq = vd->seq) & 1) cpu_pause();
        __asm__ volatile("" ::: "memory");
        clocksource_t *cs = tk.cs;
        u64 now   = cs->read();
        u64 last  = vd->cycle_last;
        u64 delta = (now - last) & vd->mask;
        if (cs == &g_cs_tsc && (s64)(now - last) < 0) delta = 0;
        u32 mult  = (clk == VDSO_CLOCK_MONOTONIC_RAW) ? vd->raw_mult : vd->mult;
        sec = vd->basetime[clk].sec;
        ns  = (vd->basetime[clk].nsec + delta * mult) >> vd->shift;
        __asm__ volatile("" ::: "memory");
    } while (vd->seq != seq);

    while (ns >= NSEC_PER_SEC) {
        ns -= NSEC_PER_SEC;
        sec++;
    }
    *sec_out = sec;
    *ns_out  = ns;
}

static void tk_read_coarse(u32 clk, u64 *sec_out, u64 *ns_out)
{
    const struct vdso_data *vd = g_vd;
    u64 sec, ns;
    u32 seq;
    do {
        while ((seq = vd->seq) & 1) cpu_pause();
        __asm__ volatile("" ::: "memory");
        sec = vd->basetime[clk].sec;
        ns  = vd->basetime[clk].nsec;
        __asm__ volatile("" ::: "memory");
    } while (vd->seq != seq);
    *sec_out = sec;
    *ns_out  = ns;
}

int ktime_get_clock(u32 clk, s64 *sec, s64 *nsec)
{
    u64 s, n;
    switch (clk) {
    case VDSO_CLOCK_REALTIME_ALARM: clk = VDSO_CLOCK_REALTIME; break;
    case VDSO_CLOCK_BOOTTIME_ALARM: clk = VDSO_CLOCK_BOOTTIME; break;
    default: break;
    }
    if (clk >= VDSO_NR_CLOCKS) return -EINVAL;
    u32 bit = 1u << clk;
    if (!(bit & (VDSO_HRES_MASK | VDSO_RAW_MASK | VDSO_COARSE_MASK))) return -EINVAL;

    if (!g_tk_ready) {
        u64 ns = early_mono_ns();
        s = ns / NSEC_PER_SEC;
        n = ns % NSEC_PER_SEC;
        if (clk == VDSO_CLOCK_REALTIME || clk == VDSO_CLOCK_REALTIME_COARSE ||
            clk == VDSO_CLOCK_TAI) {
            rtc_time_t t;
            rtc_read_time(&t);
            s = rtc_to_unix_time(&t);
        }
    } else if (bit & VDSO_COARSE_MASK) {
        tk_read_coarse(clk, &s, &n);
    } else {
        tk_read_hres(clk, &s, &n);
    }
    *sec  = (s64)s;
    *nsec = (s64)n;
    return 0;
}

static u64 ktime_get_clock_ns(u32 clk)
{
    s64 s = 0, n = 0;
    ktime_get_clock(clk, &s, &n);
    return (u64)s * NSEC_PER_SEC + (u64)n;
}

u64 ktime_get_ns(void)      { return ktime_get_clock_ns(VDSO_CLOCK_MONOTONIC); }
u64 ktime_get_raw_ns(void)  { return ktime_get_clock_ns(VDSO_CLOCK_MONOTONIC_RAW); }
u64 ktime_get_real_ns(void) { return ktime_get_clock_ns(VDSO_CLOCK_REALTIME); }

u64 ktime_get_real_seconds(void)
{
    if (!g_tk_ready) {
        rtc_time_t t;
        rtc_read_time(&t);
        return rtc_to_unix_time(&t);
    }
    return __atomic_load_n(&g_vd->basetime[VDSO_CLOCK_REALTIME_COARSE].sec, __ATOMIC_RELAXED);
}

u32 timekeeping_clock_res_ns(u32 clk)
{
    if (clk == VDSO_CLOCK_REALTIME_ALARM) clk = VDSO_CLOCK_REALTIME;
    if (clk == VDSO_CLOCK_BOOTTIME_ALARM) clk = VDSO_CLOCK_BOOTTIME;
    if (clk >= VDSO_NR_CLOCKS) return 0;
    u32 bit = 1u << clk;
    if (bit & VDSO_COARSE_MASK) return (u32)(NSEC_PER_SEC / TK_HZ);
    if (!(bit & (VDSO_HRES_MASK | VDSO_RAW_MASK))) return 0;
    if (!g_tk_ready) return (u32)(NSEC_PER_SEC / TK_HZ);
    return g_vd->hrtimer_res;
}

/* ── Setting the clock ────────────────────────────────────────────────────── */

/* Caller holds g_tk_lock inside a seq write section, having accumulated. */
static void tk_set_real_locked(s64 sec, s64 nsec)
{
    s64 mono_ns = (s64)(tk.mono_snsec >> tk.shift);
    s64 off_sec  = sec - (s64)tk.mono_sec;
    s64 off_nsec = nsec - mono_ns;
    while (off_nsec < 0)                  { off_nsec += (s64)NSEC_PER_SEC; off_sec--; }
    while (off_nsec >= (s64)NSEC_PER_SEC) { off_nsec -= (s64)NSEC_PER_SEC; off_sec++; }
    tk.real_off_sec  = off_sec;
    tk.real_off_nsec = (u64)off_nsec;

    u64 rs, rsn;
    tk_real(&rs, &rsn);
    tk.last_real_sec = rs;
}

int timekeeping_settime(s64 sec, s64 nsec)
{
    if (sec < 0 || nsec < 0 || nsec >= (s64)NSEC_PER_SEC) return -EINVAL;
    /* Linux refuses times that would overflow a 64-bit ns ktime. */
    if (sec > (s64)(0x7FFFFFFFFFFFFFFFULL / NSEC_PER_SEC) - (s64)tk.mono_sec - 1) return -EINVAL;
    if (!g_tk_ready) return -EAGAIN;

    irqflags_t f = spinlock_lock_irqsave(&g_tk_lock);
    tk_seq_begin();
    tk_accumulate(tk.cs->read());
    tk_set_real_locked(sec, nsec);
    /* A stepped clock is by definition no longer NTP-synchronised. */
    tk.status  |= STA_UNSYNC;
    tk.maxerror = NTP_PHASE_LIMIT;
    tk.esterror = NTP_PHASE_LIMIT;
    tk_publish();
    tk_seq_end();
    spinlock_unlock_irqrestore(&g_tk_lock, f);
    return 0;
}

void timekeeping_set_tz(int minuteswest, int dsttime)
{
    irqflags_t f = spinlock_lock_irqsave(&g_tk_lock);
    tk.tz_minuteswest = minuteswest;
    tk.tz_dsttime     = dsttime;
    if (g_tk_ready) {
        tk_seq_begin();
        g_vd->tz_minuteswest = minuteswest;
        g_vd->tz_dsttime     = dsttime;
        tk_seq_end();
    }
    spinlock_unlock_irqrestore(&g_tk_lock, f);
}

void timekeeping_get_tz(int *minuteswest, int *dsttime)
{
    *minuteswest = tk.tz_minuteswest;
    *dsttime     = tk.tz_dsttime;
}

/* ── adjtimex ─────────────────────────────────────────────────────────────── */

static int tk_clock_state(void)
{
    if (tk.status & STA_UNSYNC) return TIME_ERROR;
    return tk.state;
}

int timekeeping_adjtimex(struct kernel_timex *tx, bool may_set)
{
    unsigned int modes = tx->modes;

    /* Validation first, exactly as Linux's timekeeping_validate_timex(). */
    if ((modes & ADJ_OFFSET_SINGLESHOT) == ADJ_OFFSET_SINGLESHOT) {
        if (modes & ~(unsigned)ADJ_OFFSET_SS_READ) return -EINVAL;
        if (modes != ADJ_OFFSET_SS_READ && !may_set) return -EPERM;
    } else {
        if (modes && !may_set) return -EPERM;
        if ((modes & ADJ_TICK) &&
            (tx->tick < 900000 / TK_HZ || tx->tick > 1100000 / TK_HZ))
            return -EINVAL;
    }
    if (modes & ADJ_SETOFFSET) {
        if (tx->time_usec < 0) return -EINVAL;
        if (modes & ADJ_NANO) { if (tx->time_usec >= (long)NSEC_PER_SEC) return -EINVAL; }
        else                  { if (tx->time_usec >= 1000000) return -EINVAL; }
    }
    if (!g_tk_ready) return -EAGAIN;

    irqflags_t f = spinlock_lock_irqsave(&g_tk_lock);
    tk_seq_begin();
    tk_accumulate(tk.cs->read());

    if (modes & ADJ_SETOFFSET) {
        s64 add_ns = tx->time_sec * (s64)NSEC_PER_SEC +
                     ((modes & ADJ_NANO) ? tx->time_usec : tx->time_usec * (s64)NSEC_PER_USEC);
        u64 rs, rsn;
        tk_real(&rs, &rsn);
        s64 now_ns = (s64)rs * (s64)NSEC_PER_SEC + (s64)(rsn >> tk.shift);
        s64 tgt = now_ns + add_ns;
        if (tgt >= 0) tk_set_real_locked(tgt / (s64)NSEC_PER_SEC, tgt % (s64)NSEC_PER_SEC);
    }

    if ((modes & ADJ_OFFSET_SINGLESHOT) == ADJ_OFFSET_SINGLESHOT) {
        /* adjtime(3): offset in usec; report the previous outstanding one. */
        long save = (long)(tk.slew_singleshot ? tk.slew_ns / (s64)NSEC_PER_USEC : 0);
        if (modes != ADJ_OFFSET_SS_READ) {
            tk.slew_ns = (s64)tx->offset * (s64)NSEC_PER_USEC;
            tk.slew_singleshot = tk.slew_ns != 0;
            tk.slew_frac = 0;
            tk_update_mult();
        }
        tx->offset = save;
    } else {
        if (modes & ADJ_NANO)  tk.status |= STA_NANO;
        if (modes & ADJ_MICRO) tk.status &= ~STA_NANO;
        if (modes & ADJ_STATUS) {
            /* Leaving STA_PLL drops any PLL-driven slew. */
            if ((tk.status & STA_PLL) && !(tx->status & STA_PLL) && !tk.slew_singleshot)
                tk.slew_ns = 0;
            tk.status = (tk.status & STA_RONLY) | (tx->status & ~STA_RONLY);
            if (!(tk.status & (STA_INS | STA_DEL)) && (tk.state == TIME_INS || tk.state == TIME_DEL))
                tk.state = TIME_OK;
        }
        if (modes & ADJ_MAXERROR)  tk.maxerror = tx->maxerror < 0 ? 0 :
                                                 (tx->maxerror > NTP_PHASE_LIMIT ? NTP_PHASE_LIMIT : tx->maxerror);
        if (modes & ADJ_ESTERROR)  tk.esterror = tx->esterror < 0 ? 0 :
                                                 (tx->esterror > NTP_PHASE_LIMIT ? NTP_PHASE_LIMIT : tx->esterror);
        if (modes & ADJ_TIMECONST) tk.constant = tx->constant < 0 ? 0 :
                                                 (tx->constant > 10 ? 10 : tx->constant);
        if ((modes & ADJ_TAI) && tx->constant >= 0 && tx->constant <= 0x7FFFFFFF)
            tk.tai_off = (s32)tx->constant;
        if (modes & ADJ_FREQUENCY) {
            long fr = tx->freq;
            if (fr >  MAXFREQ_SCALED) fr =  MAXFREQ_SCALED;
            if (fr < -MAXFREQ_SCALED) fr = -MAXFREQ_SCALED;
            tk.freq = fr;
        }
        if (modes & ADJ_TICK) tk.tick_usec = tx->tick;
        if ((modes & ADJ_OFFSET) && (tk.status & STA_PLL)) {
            /* The PLL's phase correction: slew the measured offset away at
             * the maximum rate rather than modelling the loop filter. */
            s64 off = (tk.status & STA_NANO) ? tx->offset : tx->offset * (s64)NSEC_PER_USEC;
            const s64 maxphase = 500000000LL;   /* MAXPHASE: 0.5 s */
            if (off >  maxphase) off =  maxphase;
            if (off < -maxphase) off = -maxphase;
            tk.slew_ns = off;
            tk.slew_singleshot = false;
            tk.slew_frac = 0;
        }
        tk_update_mult();

        /* Report the current PLL offset in the caller's units. */
        s64 rem = tk.slew_singleshot ? 0 : tk.slew_ns;
        tx->offset = (long)((tk.status & STA_NANO) ? rem : rem / (s64)NSEC_PER_USEC);
    }

    tx->freq      = tk.freq;
    tx->maxerror  = tk.maxerror;
    tx->esterror  = tk.esterror;
    tx->status    = tk.status;
    tx->constant  = tk.constant;
    tx->precision = (long)(tk.cs->freq_hz >= 1000000 ? 1 : (1000000 / tk.cs->freq_hz));
    tx->tolerance = MAXFREQ_SCALED;
    tx->tick      = tk.tick_usec;
    tx->ppsfreq = tx->jitter = tx->stabil = 0;
    tx->jitcnt = tx->calcnt = tx->errcnt = tx->stbcnt = 0;
    tx->shift = 0;
    tx->tai   = tk.tai_off;

    u64 rs, rsn;
    tk_real(&rs, &rsn);
    tx->time_sec  = (long)rs;
    tx->time_usec = (long)((tk.status & STA_NANO) ? (rsn >> tk.shift)
                                                  : (rsn >> tk.shift) / NSEC_PER_USEC);

    int state = tk_clock_state();
    tk_publish();
    tk_seq_end();
    spinlock_unlock_irqrestore(&g_tk_lock, f);
    return state;
}

/* ── Clocksource selection ────────────────────────────────────────────────── */

static void tk_switch_locked(clocksource_t *cs)
{
    /* Close out the old source, then re-express the accumulated shifted
     * nanoseconds in the new source's shift. */
    tk_accumulate(tk.cs->read());
    u32 old_shift = tk.shift;
    u64 mono_ns = tk.mono_snsec >> old_shift;
    u64 raw_ns  = tk.raw_snsec  >> old_shift;

    tk.cs = cs;
    tk_setup_rate(false);
    tk.cycle_last = cs->read();
    tk.mono_snsec = mono_ns << tk.shift;
    tk.raw_snsec  = raw_ns  << tk.shift;
    tk.slew_frac  = 0;
    tk_refine_start();
}

const char *timekeeping_current_clocksource(void)
{
    return tk.cs ? tk.cs->name : "jiffies";
}

size_t timekeeping_available_clocksources(char *buf, size_t len)
{
    size_t off = 0;
    for (size_t i = 0; i < NR_CLOCKSOURCES; i++) {
        if (!g_clocksources[i]->usable) continue;
        off += (size_t)scnprintf(buf + off, len - off, "%s%s",
                                 off ? " " : "", g_clocksources[i]->name);
        if (off >= len) break;
    }
    if (off < len) off += (size_t)scnprintf(buf + off, len - off, "\n");
    return off;
}

int timekeeping_select_clocksource(const char *name)
{
    size_t n = strlen(name);
    while (n && (name[n - 1] == '\n' || name[n - 1] == ' ')) n--;
    for (size_t i = 0; i < NR_CLOCKSOURCES; i++) {
        clocksource_t *cs = g_clocksources[i];
        if (strlen(cs->name) != n || strncmp(cs->name, name, n) != 0) continue;
        if (!cs->usable) return -EINVAL;
        if (!g_tk_ready) return -EAGAIN;
        irqflags_t f = spinlock_lock_irqsave(&g_tk_lock);
        if (tk.cs != cs) {
            tk_seq_begin();
            tk_switch_locked(cs);
            tk_publish();
            tk_seq_end();
            kprintf("[TIME] Switched to clocksource %s\n", cs->name);
        }
        spinlock_unlock_irqrestore(&g_tk_lock, f);
        return 0;
    }
    return -ENODEV;
}

phys_addr_t timekeeping_vvar_phys(void)      { return g_vvar_phys; }
phys_addr_t timekeeping_hpet_page_phys(void) { return g_hpet_page_phys; }

/* ── Init ─────────────────────────────────────────────────────────────────── */

void timekeeping_init(void)
{
    g_vvar_phys = pmm_alloc_page_zeroed();
    if (!g_vvar_phys) {
        kprintf("[TIME] Out of memory for the vvar page; staying on the tick\n");
        return;
    }
    g_vd = (struct vdso_data *)PHYS_TO_VIRT(g_vvar_phys);

    /* Which sources exist. */
    u32 tsc_khz = lapic_tsc_khz();
    bool vm = (g_cpu_info.features & CPU_FEAT_HYPERVISOR) != 0;
    if (tsc_khz) {
        g_cs_tsc.freq_hz = (u64)tsc_khz * 1000ULL;
        /* An invariant TSC is the reference clock of every modern x86. A
         * non-invariant one is still constant-rate under a hypervisor (the
         * host virtualises it), but ranks below the HPET there until the
         * administrator picks it through sysfs. On bare metal without the
         * invariant bit it stops in deep C-states: never use it. */
        if (g_cpu_info.has_invariant_tsc) {
            g_cs_tsc.usable = true;
        } else if (vm) {
            g_cs_tsc.usable = true;
            if (hpet_available()) g_cs_tsc.rating = 200;
        }
    }
    if (hpet_available() && hpet_frequency_hz()) {
        g_cs_hpet.freq_hz = hpet_frequency_hz();
        g_cs_hpet.mask    = hpet_counter_mask();
        g_cs_hpet.usable  = true;
        phys_addr_t ctr   = hpet_counter_phys();
        g_hpet_page_phys  = ctr & ~(phys_addr_t)0xFFF;
        g_vd->hpet_offset = (u32)(ctr & 0xFFF);
    }

    clocksource_t *best = &g_cs_jiffies;
    for (size_t i = 0; i < NR_CLOCKSOURCES; i++)
        if (g_clocksources[i]->usable && g_clocksources[i]->rating > best->rating)
            best = g_clocksources[i];

    memset(&tk, 0, sizeof(tk));
    tk.cs        = best;
    tk.tick_usec = 1000000 / TK_HZ;
    tk.status    = STA_UNSYNC;
    tk.state     = TIME_OK;
    tk.maxerror  = NTP_PHASE_LIMIT;
    tk.esterror  = NTP_PHASE_LIMIT;
    tk.constant  = 2;
    tk_setup_rate(false);

    /* CLOCK_MONOTONIC continues from whatever the early clock said, so
     * nothing that sampled it before now sees a step backwards. */
    u64 early = early_mono_ns();
    tk.mono_sec   = early / NSEC_PER_SEC;
    tk.mono_snsec = (early % NSEC_PER_SEC) << tk.shift;
    tk.raw_sec    = tk.mono_sec;
    tk.raw_snsec  = tk.mono_snsec;
    tk.cycle_last = best->read();

    rtc_time_t t;
    rtc_read_time(&t);
    tk_set_real_locked((s64)rtc_to_unix_time(&t), 0);

    if (g_cpu_info.has_rdpid)       g_vd->getcpu_mode = VDSO_GETCPU_RDPID;
    else if (g_cpu_info.has_rdtscp) g_vd->getcpu_mode = VDSO_GETCPU_RDTSCP;
    else                            g_vd->getcpu_mode = VDSO_GETCPU_SYSCALL;

    tk_refine_start();

    tk_seq_begin();
    tk_publish();
    tk_seq_end();
    __atomic_store_n(&g_tk_ready, true, __ATOMIC_RELEASE);

    kprintf("[TIME] Clocksource %s (%llu Hz, mult=%u shift=%u)%s; realtime %llu\n",
            best->name, (unsigned long long)best->freq_hz, tk.base_mult, tk.shift,
            best->vdso_mode != VDSO_CLOCKMODE_NONE ? ", vDSO fast path" : "",
            (unsigned long long)ktime_get_real_seconds());
}
