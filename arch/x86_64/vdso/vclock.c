/* ============================================================================
 * AzamiOS — user-mode vDSO: clock_gettime / gettimeofday / time / getcpu
 * File: arch/x86_64/vdso/vclock.c
 *
 * Linked into linux-vdso.so.1 (see vdso.lds / vdso.map) and mapped by the
 * kernel into every process; libc finds it through AT_SYSINFO_EHDR. musl and
 * glibc both look these symbols up under version LINUX_2.6 and call them in
 * place of the syscall, which turns the hottest time queries in any program
 * into a few dozen instructions with no ring transition.
 *
 * Everything here runs in ring 3 against the read-only vvar page, so it must
 * never write global state, never touch the stack protector (there is no TLS
 * contract with the caller), and must fall back to the real syscall whenever
 * the kernel says no user-readable counter is available.
 * ============================================================================ */

#include "../../../include/azami/vdso.h"

typedef unsigned long long u64;
typedef long long          s64;
typedef unsigned int       u32;

struct kernel_timespec { long tv_sec; long tv_nsec; };
struct kernel_timeval  { long tv_sec; long tv_usec; };
struct kernel_timezone { int tz_minuteswest; int tz_dsttime; };

#define NR_gettimeofday   96
#define NR_time           201
#define NR_clock_gettime  228
#define NR_clock_getres   229
#define NR_getcpu         309

#define barrier() __asm__ volatile("" ::: "memory")

/* The ELF header is the first byte of the image; the kernel maps the vvar
 * page two pages below it and the HPET page directly below it. Addressing
 * them RIP-relative to the linker-provided __ehdr_start keeps this free of
 * relocations and GOT entries. The LEA is written in asm because, to the
 * compiler, pointing below a declared object is out-of-bounds arithmetic it
 * would warn about (and may assume never happens). */
static inline const struct vdso_data *vdata(void)
{
    const struct vdso_data *p;
    __asm__("lea __ehdr_start-8192(%%rip), %0" : "=r"(p));
    return p;
}

static inline const volatile char *hpet_page(void)
{
    const volatile char *p;
    __asm__("lea __ehdr_start-4096(%%rip), %0" : "=r"(p));
    return p;
}

static inline long vsyscall2(long nr, long a, long b)
{
    long ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(nr), "D"(a), "S"(b)
                     : "rcx", "r11", "memory");
    return ret;
}

static inline long vsyscall3(long nr, long a, long b, long c)
{
    long ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(nr), "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory");
    return ret;
}

static inline u32 read_seq_begin(const struct vdso_data *vd)
{
    u32 s;
    for (;;) {
        s = vd->seq;
        if (!(s & 1)) break;
        __asm__ volatile("pause");
    }
    barrier();
    return s;
}

static inline int read_seq_retry(const struct vdso_data *vd, u32 s)
{
    barrier();
    return vd->seq != s;
}

/* The counter read. LFENCE keeps RDTSC from executing ahead of the seq
 * load above it — without it the CPU may sample the TSC before it has seen
 * the kernel's new cycle_last, and the delta goes negative. */
static inline u64 read_counter(const struct vdso_data *vd, int mode)
{
    if (mode == VDSO_CLOCKMODE_TSC) {
        u32 lo, hi;
        __asm__ volatile("lfence\n\trdtsc" : "=a"(lo), "=d"(hi) :: "memory");
        return ((u64)hi << 32) | lo;
    }
    /* HPET: the main counter register, uncached MMIO. */
    const volatile u64 *ctr = (const volatile u64 *)(hpet_page() + vd->hpet_offset);
    return *ctr;
}

/* Returns 0 on success, -1 if the caller must fall back to the syscall. */
static int do_hres(const struct vdso_data *vd, u32 clk, struct kernel_timespec *ts)
{
    u64 sec, ns;
    u32 seq;
    do {
        seq = read_seq_begin(vd);
        int mode = vd->clock_mode;
        if (mode == VDSO_CLOCKMODE_NONE) return -1;
        u64 now   = read_counter(vd, mode);
        u64 last  = vd->cycle_last;
        u64 delta = (now - last) & vd->mask;
        /* A TSC read on a CPU that lags the one that last updated the
         * timekeeper by a few cycles must not run time backwards. */
        if (mode == VDSO_CLOCKMODE_TSC && (s64)(now - last) < 0) delta = 0;
        u32 mult  = (clk == VDSO_CLOCK_MONOTONIC_RAW) ? vd->raw_mult : vd->mult;
        sec = vd->basetime[clk].sec;
        ns  = vd->basetime[clk].nsec + delta * mult;
        ns >>= vd->shift;
    } while (read_seq_retry(vd, seq));

    /* ns is at most a couple of ticks' worth of seconds: iterate, don't divide. */
    while (ns >= VDSO_NSEC_PER_SEC) {
        ns -= VDSO_NSEC_PER_SEC;
        sec++;
    }
    ts->tv_sec  = (long)sec;
    ts->tv_nsec = (long)ns;
    return 0;
}

static void do_coarse(const struct vdso_data *vd, u32 clk, struct kernel_timespec *ts)
{
    u64 sec, ns;
    u32 seq;
    do {
        seq = read_seq_begin(vd);
        sec = vd->basetime[clk].sec;
        ns  = vd->basetime[clk].nsec;
    } while (read_seq_retry(vd, seq));
    ts->tv_sec  = (long)sec;
    ts->tv_nsec = (long)ns;
}

int __vdso_clock_gettime(int clock, struct kernel_timespec *ts)
{
    const struct vdso_data *vd = vdata();
    u32 clk = (u32)clock;
    if (clk < VDSO_NR_CLOCKS) {
        u32 bit = 1u << clk;
        if (bit & (VDSO_HRES_MASK | VDSO_RAW_MASK)) {
            if (do_hres(vd, clk, ts) == 0) return 0;
        } else if (bit & VDSO_COARSE_MASK) {
            do_coarse(vd, clk, ts);
            return 0;
        }
    }
    return (int)vsyscall2(NR_clock_gettime, clock, (long)ts);
}

int __vdso_gettimeofday(struct kernel_timeval *tv, struct kernel_timezone *tz)
{
    const struct vdso_data *vd = vdata();
    if (tv) {
        struct kernel_timespec ts;
        if (do_hres(vd, VDSO_CLOCK_REALTIME, &ts) != 0)
            return (int)vsyscall2(NR_gettimeofday, (long)tv, (long)tz);
        tv->tv_sec  = ts.tv_sec;
        tv->tv_usec = ts.tv_nsec / 1000;
    }
    if (tz) {
        tz->tz_minuteswest = vd->tz_minuteswest;
        tz->tz_dsttime     = vd->tz_dsttime;
    }
    return 0;
}

long __vdso_time(long *t)
{
    const struct vdso_data *vd = vdata();
    /* time(2) is seconds only: the coarse realtime second is exact enough
     * (it is the same second the hi-res clock reports, up to one tick). A
     * single aligned 64-bit load needs no sequence loop. */
    long sec = (long)*(const volatile u64 *)&vd->basetime[VDSO_CLOCK_REALTIME_COARSE].sec;
    if (t) *t = sec;
    return sec;
}

int __vdso_clock_getres(int clock, struct kernel_timespec *res)
{
    const struct vdso_data *vd = vdata();
    u32 clk = (u32)clock;
    if (clk < VDSO_NR_CLOCKS) {
        u32 bit = 1u << clk;
        u32 ns = 0;
        if (bit & (VDSO_HRES_MASK | VDSO_RAW_MASK)) ns = vd->hrtimer_res;
        else if (bit & VDSO_COARSE_MASK)          ns = vd->coarse_res;
        if (ns) {
            if (res) { res->tv_sec = 0; res->tv_nsec = ns; }
            return 0;
        }
    }
    return (int)vsyscall2(NR_clock_getres, clock, (long)res);
}

long __vdso_getcpu(unsigned *cpu, unsigned *node, void *unused)
{
    const struct vdso_data *vd = vdata();
    u32 p;
    switch (vd->getcpu_mode) {
    case VDSO_GETCPU_RDPID: {
        u64 v;
        __asm__ volatile(".byte 0xf3, 0x0f, 0xc7, 0xf8" : "=a"(v)); /* rdpid %rax */
        p = (u32)v;
        break;
    }
    case VDSO_GETCPU_RDTSCP: {
        u32 lo, hi;
        __asm__ volatile("rdtscp" : "=a"(lo), "=d"(hi), "=c"(p));
        (void)lo; (void)hi;
        break;
    }
    default:
        return vsyscall3(NR_getcpu, (long)cpu, (long)node, (long)unused);
    }
    /* IA32_TSC_AUX = (node << 12) | cpu, the Linux encoding. */
    if (cpu)  *cpu  = p & 0xfff;
    if (node) *node = p >> 12;
    return 0;
}

/* The unprefixed names Linux also exports from its x86_64 vDSO. */
int  clock_gettime(int, struct kernel_timespec *)
        __attribute__((weak, alias("__vdso_clock_gettime")));
int  gettimeofday(struct kernel_timeval *, struct kernel_timezone *)
        __attribute__((weak, alias("__vdso_gettimeofday")));
long time(long *)
        __attribute__((weak, alias("__vdso_time")));
int  clock_getres(int, struct kernel_timespec *)
        __attribute__((weak, alias("__vdso_clock_getres")));
long getcpu(unsigned *, unsigned *, void *)
        __attribute__((weak, alias("__vdso_getcpu")));
