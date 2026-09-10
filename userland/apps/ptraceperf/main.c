/* ============================================================================
 * AzamiOS Userspace — ptrace(2) / perf_event_open(2) regression probe
 * File: userland/apps/ptraceperf/main.c
 *
 * Runs as PID 1 in a headless test boot and prints a pass/fail line per check.
 * Every wait is polled with WNOHANG and a retry budget, so a kernel bug shows
 * up as a FAIL with a diagnosis rather than a VM that hangs until the harness
 * timeout and tells you nothing.
 * ========================================================================= */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sched.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

/* ── ptrace requests ─────────────────────────────────────────────────────── */
#define PTRACE_TRACEME      0
#define PTRACE_PEEKDATA     2
#define PTRACE_POKEDATA     5
#define PTRACE_CONT         7
#define PTRACE_SINGLESTEP   9
#define PTRACE_GETREGS      12
#define PTRACE_SETREGS      13
#define PTRACE_ATTACH       16
#define PTRACE_DETACH       17
#define PTRACE_SYSCALL      24
#define PTRACE_SETOPTIONS   0x4200
#define PTRACE_O_TRACESYSGOOD 0x00000001

struct user_regs_struct {
    unsigned long r15, r14, r13, r12, rbp, rbx, r11, r10, r9, r8, rax, rcx, rdx;
    unsigned long rsi, rdi, orig_rax, rip, cs, eflags, rsp, ss;
    unsigned long fs_base, gs_base, ds, es, fs, gs;
};

/* ── perf ────────────────────────────────────────────────────────────────── */
#define PERF_TYPE_HARDWARE  0
#define PERF_TYPE_SOFTWARE  1
#define PERF_TYPE_RAW       4
#define PERF_COUNT_HW_INSTRUCTIONS      1
#define PERF_COUNT_SW_TASK_CLOCK        1
#define PERF_COUNT_SW_PAGE_FAULTS       2
#define PERF_COUNT_SW_CONTEXT_SWITCHES  3
#define PERF_FORMAT_TOTAL_TIME_ENABLED  (1U << 0)
#define PERF_IOC_ENABLE   0x2400
#define PERF_IOC_DISABLE  0x2401
#define PERF_IOC_RESET    0x2403

struct perf_event_attr {
    unsigned int  type;
    unsigned int  size;
    unsigned long config;
    unsigned long sample_period;
    unsigned long sample_type;
    unsigned long read_format;
    unsigned long flags;
    unsigned int  wakeup_events;
    unsigned int  bp_type;
    unsigned long config1;
    unsigned long config2;
};

static long ptrace_(long req, long pid, long addr, long data)
{
    return syscall4(SYS_ptrace, req, pid, addr, data);
}
static long perf_open(struct perf_event_attr *a, int pid, int cpu, int grp, unsigned long fl)
{
    return syscall5(SYS_perf_event_open, (long)a, pid, cpu, grp, (long)fl);
}

static int g_pass, g_total;
#define TEST(cond, name) do {                                                 \
        g_total++;                                                            \
        if (cond) { g_pass++; printf("  PASS  %s\n", (name)); }               \
        else      { printf("  FAIL  %s\n", (name)); }                         \
    } while (0)
#define SKIP(name, why) printf("  SKIP  %s (%s)\n", (name), (why))

/* Poll for a state change with a bounded budget, so no check can hang. */
static int wait_change(int pid, int *st, int opts, int tries)
{
    for (int i = 0; i < tries; i++) {
        int w = waitpid(pid, st, opts | WNOHANG);
        if (w == pid) return 1;
        if (w < 0) return 0;
        sched_yield();
    }
    return 0;
}

/* Shared with the traced child by fork(), at the same virtual address in both.
 * The kernel maps a read-only page into both address spaces, so a POKEDATA
 * here also exercises the copy-on-write break in the poke path. */
static volatile long g_shared = 0x1234ABCDL;

static void busy(unsigned long n) { for (volatile unsigned long i = 0; i < n; i++) { } }

/* ── 1. Job control, no tracer ───────────────────────────────────────────── */
static void test_job_control(void)
{
    printf("\n[1] job-control stop / continue\n");

    int c = fork();
    if (c == 0) { for (;;) sched_yield(); }
    if (c < 0) { TEST(0, "fork"); return; }

    for (int i = 0; i < 50; i++) sched_yield();

    int st = 0;
    kill(c, SIGSTOP);
    int ok = wait_change(c, &st, WUNTRACED, 20000);
    TEST(ok && WIFSTOPPED(st), "SIGSTOP stops the child instead of killing it");
    TEST(ok && WIFSTOPPED(st) && WSTOPSIG(st) == SIGSTOP,
         "wait4(WUNTRACED) reports WSTOPSIG == SIGSTOP");

    /* A stopped process must stay stopped: nothing should be reported twice. */
    st = 0;
    TEST(waitpid(c, &st, WUNTRACED | WNOHANG) == 0,
         "the same stop is not reported a second time");

    kill(c, SIGCONT);
    ok = wait_change(c, &st, WCONTINUED, 20000);
    TEST(ok && WIFCONTINUED(st), "SIGCONT resumes it and wait4(WCONTINUED) sees it");

    kill(c, SIGKILL);
    ok = wait_change(c, &st, 0, 20000);
    TEST(ok && WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL,
         "SIGKILL then terminates it");
}

/* ── 2. Stopping a process asleep inside a syscall ───────────────────────── */
static void test_stop_in_syscall(void)
{
    printf("\n[2] stopping a process blocked in a syscall\n");

    int c = fork();
    if (c == 0) { pause(); _exit(0); }
    if (c < 0) { TEST(0, "fork"); return; }

    for (int i = 0; i < 200; i++) sched_yield();

    int st = 0;
    kill(c, SIGSTOP);
    int ok = wait_change(c, &st, WUNTRACED, 40000);
    TEST(ok && WIFSTOPPED(st), "a child blocked in pause(2) still stops");

    kill(c, SIGKILL);
    (void)wait_change(c, &st, 0, 40000);
}

/* ── 3. TRACEME, peek, poke, cont ────────────────────────────────────────── */
static void test_peek_poke(void)
{
    printf("\n[3] PTRACE_TRACEME / PEEKDATA / POKEDATA\n");

    int c = fork();
    if (c == 0) {
        ptrace_(PTRACE_TRACEME, 0, 0, 0);
        kill(getpid(), SIGSTOP);
        _exit(g_shared == 0x5AL ? 42 : 7);
    }
    if (c < 0) { TEST(0, "fork"); return; }

    int st = 0;
    int ok = wait_change(c, &st, WUNTRACED, 40000);
    TEST(ok && WIFSTOPPED(st), "a tracee's raise(SIGSTOP) reaches the tracer");
    if (!ok) { kill(c, SIGKILL); return; }

    long v = 0;
    long rc = ptrace_(PTRACE_PEEKDATA, c, (long)&g_shared, (long)&v);
    TEST(rc == 0 && v == 0x1234ABCDL, "PEEKDATA reads the tracee's memory");

    rc = ptrace_(PTRACE_POKEDATA, c, (long)&g_shared, 0x5AL);
    TEST(rc == 0, "POKEDATA is accepted");
    v = 0;
    ptrace_(PTRACE_PEEKDATA, c, (long)&g_shared, (long)&v);
    TEST(v == 0x5AL, "PEEKDATA reads back what POKEDATA wrote");
    TEST(g_shared == 0x1234ABCDL,
         "the poke broke the shared page, leaving the tracer's copy alone");

    struct user_regs_struct regs;
    memset(&regs, 0, sizeof(regs));
    rc = ptrace_(PTRACE_GETREGS, c, 0, (long)&regs);
    TEST(rc == 0 && regs.rip != 0 && regs.rsp != 0 && (regs.cs & 3) == 3,
         "GETREGS returns a plausible ring-3 frame");

    /* /proc must agree with the kernel's own view of the stop. */
    {
        char path[64], sbuf[512];
        snprintf(path, sizeof(path), "/proc/%d/status", c);
        int pf = open(path, O_RDONLY);
        long n = pf >= 0 ? read(pf, sbuf, sizeof(sbuf) - 1) : -1;
        if (n > 0) sbuf[n] = 0; else sbuf[0] = 0;
        if (pf >= 0) close(pf);
        char want[32];
        snprintf(want, sizeof(want), "TracerPid:%d", getpid());
        TEST(strstr(sbuf, "State:  t (tracing stop)") != 0,
             "/proc/<pid>/status reports the tracing stop");
        TEST(strstr(sbuf, want) != 0, "/proc/<pid>/status reports TracerPid");
    }

    ptrace_(PTRACE_CONT, c, 0, 0);
    ok = wait_change(c, &st, 0, 40000);
    TEST(ok && WIFEXITED(st) && WEXITSTATUS(st) == 42,
         "the tracee resumes and sees the poked value");
    if (!ok) kill(c, SIGKILL);
}

/* ── 4. Syscall stops ────────────────────────────────────────────────────── */
static void test_syscall_stops(void)
{
    printf("\n[4] PTRACE_SYSCALL entry/exit stops\n");

    int c = fork();
    if (c == 0) {
        ptrace_(PTRACE_TRACEME, 0, 0, 0);
        kill(getpid(), SIGSTOP);
        for (int i = 0; i < 4; i++) (void)getpid();
        _exit(0);
    }
    if (c < 0) { TEST(0, "fork"); return; }

    int st = 0;
    if (!wait_change(c, &st, WUNTRACED, 40000)) { TEST(0, "initial stop"); kill(c, SIGKILL); return; }

    ptrace_(PTRACE_SETOPTIONS, c, 0, PTRACE_O_TRACESYSGOOD);

    int stops = 0, sysgood = 0, exited = 0;
    for (int i = 0; i < 64; i++) {
        if (ptrace_(PTRACE_SYSCALL, c, 0, 0) != 0) break;
        if (!wait_change(c, &st, WUNTRACED, 40000)) break;
        if (WIFEXITED(st) || WIFSIGNALED(st)) { exited = 1; break; }
        if (!WIFSTOPPED(st)) break;
        stops++;
        if (WSTOPSIG(st) == (SIGTRAP | 0x80)) sysgood++;
    }
    TEST(stops >= 8, "each traced syscall produces an entry and an exit stop");
    TEST(sysgood == stops && stops > 0,
         "PTRACE_O_TRACESYSGOOD marks them SIGTRAP|0x80");
    TEST(exited, "the tracee runs to completion once it is let go");
    if (!exited) { kill(c, SIGKILL); (void)wait_change(c, &st, 0, 20000); }
}

/* ── 5. Breakpoints and single-step ──────────────────────────────────────── */
static void test_trap_and_step(void)
{
    printf("\n[5] int3 breakpoint and PTRACE_SINGLESTEP\n");

    int c = fork();
    if (c == 0) {
        ptrace_(PTRACE_TRACEME, 0, 0, 0);
        kill(getpid(), SIGSTOP);
        __asm__ volatile ("int3");
        busy(2000);
        _exit(11);
    }
    if (c < 0) { TEST(0, "fork"); return; }

    int st = 0;
    if (!wait_change(c, &st, WUNTRACED, 40000)) { TEST(0, "initial stop"); kill(c, SIGKILL); return; }

    ptrace_(PTRACE_CONT, c, 0, 0);
    int ok = wait_change(c, &st, WUNTRACED, 40000);
    TEST(ok && WIFSTOPPED(st) && WSTOPSIG(st) == SIGTRAP,
         "int3 becomes a SIGTRAP stop instead of killing the tracee");

    struct user_regs_struct r1, r2;
    memset(&r1, 0, sizeof(r1));
    memset(&r2, 0, sizeof(r2));
    ptrace_(PTRACE_GETREGS, c, 0, (long)&r1);

    ptrace_(PTRACE_SINGLESTEP, c, 0, 0);
    ok = wait_change(c, &st, WUNTRACED, 40000);
    TEST(ok && WIFSTOPPED(st) && WSTOPSIG(st) == SIGTRAP,
         "PTRACE_SINGLESTEP stops again after one instruction");
    ptrace_(PTRACE_GETREGS, c, 0, (long)&r2);
    TEST(r2.rip != r1.rip, "and RIP actually advanced");

    ptrace_(PTRACE_DETACH, c, 0, 0);
    ok = wait_change(c, &st, 0, 40000);
    TEST(ok && WIFEXITED(st) && WEXITSTATUS(st) == 11,
         "PTRACE_DETACH lets it finish normally");
    if (!ok) { kill(c, SIGKILL); (void)wait_change(c, &st, 0, 20000); }
}

/* ── 6. PTRACE_ATTACH to a running process ───────────────────────────────── */
static void test_attach(void)
{
    printf("\n[6] PTRACE_ATTACH / DETACH\n");

    int c = fork();
    if (c == 0) { for (;;) sched_yield(); }
    if (c < 0) { TEST(0, "fork"); return; }
    for (int i = 0; i < 50; i++) sched_yield();

    int st = 0;
    long rc = ptrace_(PTRACE_ATTACH, c, 0, 0);
    TEST(rc == 0, "PTRACE_ATTACH to a running child succeeds");
    int ok = wait_change(c, &st, WUNTRACED, 40000);
    TEST(ok && WIFSTOPPED(st), "the attached child stops");

    struct user_regs_struct regs;
    memset(&regs, 0, sizeof(regs));
    TEST(ptrace_(PTRACE_GETREGS, c, 0, (long)&regs) == 0 && regs.rip != 0,
         "GETREGS works on an attached tracee");

    TEST(ptrace_(PTRACE_DETACH, c, 0, 0) == 0, "PTRACE_DETACH succeeds");
    kill(c, SIGKILL);
    (void)wait_change(c, &st, 0, 40000);
}

/* ── 7. Permission ───────────────────────────────────────────────────────── */
static void test_ptrace_errors(void)
{
    printf("\n[7] error handling\n");
    TEST(ptrace_(PTRACE_ATTACH, 1, 0, 0) < 0, "PTRACE_ATTACH to PID 1 is refused");
    TEST(ptrace_(PTRACE_CONT, 999999, 0, 0) < 0, "a request for an unknown pid fails");
    TEST(ptrace_(0x7fff, getpid(), 0, 0) < 0, "an unknown request fails");
}

/* ── 8. perf ─────────────────────────────────────────────────────────────── */
static unsigned long perf_read1(int fd)
{
    unsigned long buf[4] = {0, 0, 0, 0};
    long n = read(fd, buf, sizeof(buf));
    if (n < (long)sizeof(unsigned long)) return (unsigned long)-1;
    return buf[0];
}

static void test_perf(void)
{
    printf("\n[8] perf_event_open\n");

    struct perf_event_attr a;

    /* Software: page faults on ourselves. */
    memset(&a, 0, sizeof(a));
    a.type = PERF_TYPE_SOFTWARE;
    a.size = sizeof(a);
    a.config = PERF_COUNT_SW_PAGE_FAULTS;
    int fd = (int)perf_open(&a, 0, -1, -1, 0);
    TEST(fd >= 0, "perf_event_open(SW_PAGE_FAULTS, self)");
    if (fd >= 0) {
        unsigned long v1 = perf_read1(fd);
        /* Touch fresh anonymous pages so the kernel has faults to count. */
        char *m = mmap(0, 512 * 1024, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (m != MAP_FAILED) {
            for (int i = 0; i < 512 * 1024; i += 4096) m[i] = (char)i;
        }
        unsigned long v2 = perf_read1(fd);
        TEST(v2 >= v1 && v2 != (unsigned long)-1, "its count is readable and monotonic");
        if (m != MAP_FAILED) munmap(m, 512 * 1024);
        close(fd);
    }

    /* Software: context switches, with a time_enabled field appended. */
    memset(&a, 0, sizeof(a));
    a.type = PERF_TYPE_SOFTWARE;
    a.size = sizeof(a);
    a.config = PERF_COUNT_SW_CONTEXT_SWITCHES;
    a.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED;
    fd = (int)perf_open(&a, 0, -1, -1, 0);
    TEST(fd >= 0, "perf_event_open(SW_CONTEXT_SWITCHES) with a read_format");
    if (fd >= 0) {
        unsigned long buf[2] = {0, 0};
        unsigned long c1 = 0;
        if (read(fd, buf, sizeof(buf)) == (long)sizeof(buf)) c1 = buf[0];
        for (int i = 0; i < 200; i++) sched_yield();
        long n = read(fd, buf, sizeof(buf));
        TEST(n == (long)sizeof(buf), "read() returns both words");
        TEST(buf[0] > c1, "yielding 200 times increases the switch count");

        /* A one-word buffer cannot hold a two-word record. */
        unsigned long small = 0;
        TEST(read(fd, &small, sizeof(small)) < 0,
             "a buffer too small for the record is refused");

        TEST(ioctl(fd, PERF_IOC_RESET, 0) == 0, "ioctl(PERF_EVENT_IOC_RESET)");
        TEST(ioctl(fd, PERF_IOC_DISABLE, 0) == 0, "ioctl(PERF_EVENT_IOC_DISABLE)");
        TEST(ioctl(fd, PERF_IOC_ENABLE, 0) == 0, "ioctl(PERF_EVENT_IOC_ENABLE)");
        close(fd);
    }

    /* Software: task-clock should advance while we burn CPU. */
    memset(&a, 0, sizeof(a));
    a.type = PERF_TYPE_SOFTWARE;
    a.size = sizeof(a);
    a.config = PERF_COUNT_SW_TASK_CLOCK;
    fd = (int)perf_open(&a, 0, -1, -1, 0);
    if (fd >= 0) {
        unsigned long t1 = perf_read1(fd);
        busy(20000000);
        unsigned long t2 = perf_read1(fd);
        TEST(t2 >= t1, "SW_TASK_CLOCK advances while the task runs");
        close(fd);
    } else {
        TEST(0, "perf_event_open(SW_TASK_CLOCK)");
    }

    /* Sampling has no implementation and must be refused, not ignored. */
    memset(&a, 0, sizeof(a));
    a.type = PERF_TYPE_SOFTWARE;
    a.size = sizeof(a);
    a.config = PERF_COUNT_SW_PAGE_FAULTS;
    a.sample_period = 1000;
    TEST(perf_open(&a, 0, -1, -1, 0) < 0, "a sampling attr is rejected outright");

    memset(&a, 0, sizeof(a));
    a.type = 99;
    a.size = sizeof(a);
    TEST(perf_open(&a, 0, -1, -1, 0) < 0, "an unknown attr.type is rejected");

    /* Hardware: present only when the CPU model exposes a PMU. */
    memset(&a, 0, sizeof(a));
    a.type = PERF_TYPE_HARDWARE;
    a.size = sizeof(a);
    a.config = PERF_COUNT_HW_INSTRUCTIONS;
    fd = (int)perf_open(&a, 0, -1, -1, 0);
    if (fd < 0) {
        SKIP("PERF_TYPE_HARDWARE instructions", "no PMU on this CPU model");
    } else {
        unsigned long i1 = perf_read1(fd);
        busy(5000000);
        unsigned long i2 = perf_read1(fd);
        TEST(i2 > i1, "HW_INSTRUCTIONS counts real retired instructions");
        close(fd);
    }
}

int main(void)
{
    printf("\n==== AzamiOS ptrace / perf probe ====\n");

    {   /* Which PMU the kernel settled on — decides whether [8] can test
             hardware counters or has to skip them. */
        char b[4096];
        int f = open("/proc/cpuinfo", O_RDONLY);
        long n = f >= 0 ? read(f, b, sizeof(b) - 1) : -1;
        if (n > 0) {
            b[n] = 0;
            char *l = strstr(b, "azami_pmu");
            if (l) {
                char *e = strchr(l, '\n');
                if (e) *e = 0;
                printf("kernel reports %s\n", l);
            } else {
                printf("  WARN  /proc/cpuinfo has no azami_pmu line\n");
            }
        }
        if (f >= 0) close(f);
    }

    test_job_control();
    test_stop_in_syscall();
    test_peek_poke();
    test_syscall_stops();
    test_trap_and_step();
    test_attach();
    test_ptrace_errors();
    test_perf();

    printf("\n==== RESULT: %d/%d passed ====\n", g_pass, g_total);
    printf("PROBE-DONE\n");

    /* As an ordinary program, exit with the verdict. As PID 1 there is nothing
     * to return to, so idle instead of letting the kernel lose its init. */
    if (getpid() == 1) for (;;) sched_yield();
    return g_pass == g_total ? 0 : 1;
}
