#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/signalfd.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int passed, failed;
static volatile sig_atomic_t signals;
static volatile uint64_t handler_mask;
static int handler_self_block = 1;
#define BIT(sig) (UINT64_C(1) << ((sig) - 1))
static void record(const char *name, long value, long expected)
{
    int good = value == expected;
    printf("NSCASE {\"name\":\"%s\",\"value\":%ld,\"expected\":%ld,\"passed\":%s}\n",
           name, value, expected, good ? "true" : "false");
    if (good) passed++; else failed++;
}
static void fatal(const char *message) { perror(message); exit(2); }
static long result(long value) { return value < 0 ? -errno : value; }
static long wait2(int fd, void *events, int max, const struct timespec *ts, const void *mask, size_t size)
{ return result(syscall(SYS_epoll_pwait2, fd, events, max, ts, mask, size)); }
static long wait1(int fd, void *events, int max, int ms, const void *mask, size_t size)
{ return result(syscall(SYS_epoll_pwait, fd, events, max, ms, mask, size)); }
static uint64_t get_mask(void)
{
    uint64_t mask = 0;
    if (syscall(SYS_rt_sigprocmask, SIG_SETMASK, NULL, &mask, 8)) fatal("get mask");
    return mask;
}
static void set_mask(uint64_t mask)
{ if (syscall(SYS_rt_sigprocmask, SIG_SETMASK, &mask, NULL, 8)) fatal("set mask"); }
static void handler(int signo)
{
    (void)signo;
    uint64_t mask = 0;
    syscall(SYS_rt_sigprocmask, SIG_SETMASK, NULL, &mask, 8);
    handler_mask = mask; signals++;
}
static uint64_t now(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) fatal("monotonic");
    return (uint64_t)ts.tv_sec * 1000000000 + (uint64_t)ts.tv_nsec;
}
static void sleep_ms(int ms)
{
    struct timespec ts = {.tv_sec=ms/1000, .tv_nsec=(ms%1000)*1000000L};
    while (nanosleep(&ts, &ts) && errno == EINTR) {}
}
static void reap(pid_t child)
{
    int status;
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status)) fatal("sender child");
}
static pid_t sender(int pipefd, int signo)
{
    pid_t parent = getpid(), child = fork();
    if (child < 0) fatal("fork sender");
    if (!child) {
        sleep_ms(25);
        if (signo && kill(parent, signo)) _exit(10);
        if (pipefd >= 0 && write(pipefd, "x", 1) != 1) _exit(11);
        _exit(0);
    }
    return child;
}
static void timeout_case(int ep, const char *name, long ns)
{
    struct timespec ts = {.tv_sec=ns/1000000000, .tv_nsec=ns%1000000000};
    struct epoll_event event;
    uint64_t start = now();
    long value = wait2(ep, &event, 1, &ts, NULL, 0);
    uint64_t elapsed = now() - start;
    record(name, value == 0 && elapsed >= (uint64_t)ns && elapsed < 2000000000, 1);
    printf("EPTIME {\"name\":\"%s\",\"requested_ns\":%ld,\"elapsed_ns\":%llu}\n",
           name, ns, (unsigned long long)elapsed);
}
static void delayed_readiness(int ep, int readfd, int writefd, const char *name, struct timespec *ts)
{
    struct epoll_event out;
    pid_t child = sender(writefd, 0);
    uint64_t start = now();
    long value = wait2(ep, &out, 1, ts, NULL, 0);
    uint64_t elapsed = now() - start;
    record(name, value == 1 && (out.events & EPOLLIN) && out.data.u64 == 0x123456789abcdef0ULL &&
           elapsed >= 10000000 && elapsed < 2000000000, 1);
    reap(child);
    char c; if (read(readfd, &c, 1) != 1) fatal("drain pipe");
}
static void interruption(int ep, const char *name, int which, int pending)
{
    struct epoll_event event;
    struct timespec huge = {.tv_sec=LONG_MAX, .tv_nsec=999999999};
    uint64_t original = BIT(SIGUSR1), temporary = BIT(SIGUSR2) | BIT(SIGKILL) | BIT(SIGSTOP);
    signals = 0; handler_mask = 0;
    set_mask(original);
    pid_t child = -1;
    if (pending) { if (kill(getpid(), SIGUSR1)) fatal("pending signal"); }
    else child = sender(-1, SIGUSR1);
    uint64_t start = now();
    long value;
    if (which == 2) value = wait2(ep, &event, 1, &huge, &temporary, 8);
    else if (which == 1) value = wait1(ep, &event, 1, -1, &temporary, 8);
    else { set_mask(0); value = result(syscall(SYS_epoll_wait, ep, &event, 1, -1)); }
    record(name, value, -EINTR);
    char key[128];
    snprintf(key, sizeof(key), "%s.handler", name); record(key, signals, 1);
    uint64_t expected = (which ? BIT(SIGUSR2) : 0) | BIT(SIGTERM) |
                        (handler_self_block ? BIT(SIGUSR1) : 0);
    snprintf(key, sizeof(key), "%s.handler_mask", name); record(key, handler_mask == expected, 1);
    snprintf(key, sizeof(key), "%s.restored", name); record(key, get_mask() == (which ? original : 0), 1);
    if (!pending) {
        snprintf(key, sizeof(key), "%s.bounded", name);
        record(key, now() - start < 2000000000, 1);
        reap(child);
    }
    set_mask(0);
}
struct race_data { int ep, target, entered, done, bad; unsigned changes; };
static void *modifier(void *opaque)
{
    struct race_data *race = opaque;
    struct epoll_event event = {.events=EPOLLIN};
    while (!__atomic_load_n(&race->done, __ATOMIC_ACQUIRE)) {
        event.data.u64 = (race->changes & 1) ? 0x1122334455667788ULL : 0x8877665544332211ULL;
        if (epoll_ctl(race->ep, EPOLL_CTL_MOD, race->target, &event)) race->bad = 1;
        __atomic_add_fetch(&race->changes, 1, __ATOMIC_RELEASE);
        sched_yield();
    }
    return NULL;
}
static void *closer(void *opaque)
{
    struct race_data *race = opaque;
    while (!__atomic_load_n(&race->entered, __ATOMIC_ACQUIRE)) sched_yield();
    sleep_ms(25);
    uint64_t one = 1;
    if (close(race->ep) || write(race->target, &one, 8) != 8) race->bad = 1;
    return NULL;
}
static void race_cases(void)
{
    struct race_data race = {0};
    race.ep = epoll_create1(0); race.target = eventfd(1, EFD_NONBLOCK);
    if (race.ep < 0 || race.target < 0) fatal("race descriptors");
    struct epoll_event event = {.events=EPOLLIN, .data.u64=0x1122334455667788ULL}, out;
    if (epoll_ctl(race.ep, EPOLL_CTL_ADD, race.target, &event)) fatal("race add");
    pthread_t worker;
    if (pthread_create(&worker, NULL, modifier, &race)) fatal("modifier");
    while (__atomic_load_n(&race.changes, __ATOMIC_ACQUIRE) < 10) sched_yield();
    struct timespec zero = {0}; int good = 1;
    for (int i = 0; i < 2000; i++) {
        long n = wait2(race.ep, &out, 1, &zero, NULL, 0);
        good &= n == 1 && out.events == EPOLLIN &&
                (out.data.u64 == 0x1122334455667788ULL || out.data.u64 == 0x8877665544332211ULL);
    }
    __atomic_store_n(&race.done, 1, __ATOMIC_RELEASE);
    if (pthread_join(worker, NULL)) fatal("modifier join");
    /* Per-thread signal masks remain outside this checkpoint. A pthread's
     * exit currently blocks the process mask; restore the supervisor's mask. */
    set_mask(0);
    record("race.concurrent_ctl_snapshot", good && !race.bad, 1);
    close(race.ep); close(race.target);
    good = 1;
    for (int i = 0; i < 20; i++) {
        race = (struct race_data){.ep=epoll_create1(0), .target=eventfd(0, EFD_NONBLOCK)};
        if (race.ep < 0 || race.target < 0 || epoll_ctl(race.ep, EPOLL_CTL_ADD, race.target, &event)) fatal("close race setup");
        if (pthread_create(&worker, NULL, closer, &race)) fatal("closer");
        struct timespec limit = {.tv_sec=1};
        __atomic_store_n(&race.entered, 1, __ATOMIC_RELEASE);
        good &= wait2(race.ep, &out, 1, &limit, NULL, 0) == 1 && out.data.u64 == event.data.u64;
        if (pthread_join(worker, NULL)) fatal("closer join");
        set_mask(0); good &= !race.bad; close(race.target);
    }
    record("race.epoll_close_during_wait", good, 1);
}
int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    /* Kernel runner supervises the entire payload; bound every individual
     * infinite/overflow case too, using a fatal SIGALRM watchdog. */
    alarm(40);
    struct sigaction action = {.sa_handler=handler, .sa_flags=SA_RESTART};
    sigemptyset(&action.sa_mask); sigaddset(&action.sa_mask, SIGTERM);
    if (sigaction(SIGUSR1, &action, NULL)) fatal("handler");
    set_mask(0);
    int ep = epoll_create1(EPOLL_CLOEXEC); if (ep < 0) fatal("epoll");
    struct epoll_event out[2]; struct timespec zero = {0}, bad;
    record("create.cloexec", fcntl(ep, F_GETFD), FD_CLOEXEC);
    record("create.invalid_flags", result(syscall(SYS_epoll_create1, 1)), -EINVAL);
    record("create.invalid_size", result(syscall(SYS_epoll_create, 0)), -EINVAL);
    record("wait.bad_fd", wait2(-1, out, 1, &zero, NULL, 0), -EBADF);
    record("wait.bad_fd_precedes_maxevents", wait2(-1, out, 0, &zero, NULL, 0), -EBADF);
    record("wait.maxevents_zero", wait2(ep, out, 0, &zero, NULL, 0), -EINVAL);
    record("wait.maxevents_negative", wait2(ep, out, -1, &zero, NULL, 0), -EINVAL);
    record("wait.maxevents_overflow", wait2(ep, out, INT_MAX, &zero, NULL, 0), -EINVAL);
    record("wait.maxevents_1025", wait2(ep, out, 1025, &zero, NULL, 0), 0);
    record("wait.zero", wait2(ep, out, 1, &zero, NULL, 0), 0);
    record("wait.empty_null_events", wait2(ep, NULL, 1, &zero, NULL, 0), 0);
    record("wait.empty_unmapped_events", wait2(ep, (void *)1, 1, &zero, NULL, 0), 0);
    record("wait.noncanonical_events", wait2(ep, (void *)UINT64_MAX, 1, &zero, NULL, 0), -EFAULT);
    record("wait.events_span_overflow", wait2(ep, (void *)0x00007fffffffffffULL, 2, &zero, NULL, 0), -EFAULT);
    bad = (struct timespec){.tv_sec=-1}; record("timeout.negative_seconds", wait2(ep, out, 1, &bad, NULL, 0), -EINVAL);
    bad = (struct timespec){.tv_nsec=-1}; record("timeout.negative_nanoseconds", wait2(ep, out, 1, &bad, NULL, 0), -EINVAL);
    bad = (struct timespec){.tv_nsec=1000000000}; record("timeout.nanoseconds_limit", wait2(ep, out, 1, &bad, NULL, 0), -EINVAL);
    record("timeout.bad_pointer", wait2(ep, out, 1, (void *)1, NULL, 0), -EFAULT);
    record("timeout.validation_first", wait2(-1, out, 0, &bad, (void *)1, 0), -EINVAL);
    uint64_t mask = 0;
    record("mask.size_zero", wait2(ep, out, 1, &zero, &mask, 0), -EINVAL);
    record("mask.libc_size", wait2(ep, out, 1, &zero, &mask, sizeof(sigset_t)), -EINVAL);
    record("mask.size_precedes_pointer", wait2(ep, out, 1, &zero, (void *)1, 0), -EINVAL);
    record("mask.bad_pointer", wait2(ep, out, 1, &zero, (void *)1, 8), -EFAULT);
    record("mask.null_ignores_size", wait2(ep, out, 1, &zero, NULL, SIZE_MAX), 0);
    record("pwait.zero", wait1(ep, out, 1, 0, NULL, SIZE_MAX), 0);
    record("pwait.mask_size", wait1(ep, out, 1, 0, &mask, 0), -EINVAL);
    record("pwait.mask_pointer", wait1(ep, out, 1, 0, (void *)1, 8), -EFAULT);
    set_mask(BIT(SIGUSR1));
    mask = BIT(SIGUSR2) | BIT(SIGKILL) | BIT(SIGSTOP);
    record("mask.timeout_restored", wait2(ep, out, 1, &zero, &mask, 8) == 0 && get_mask() == BIT(SIGUSR1), 1);
    record("mask.error_restored", wait2(-1, out, 1, &zero, &mask, 8) == -EBADF && get_mask() == BIT(SIGUSR1), 1);
    set_mask(BIT(SIGUSR1) | BIT(SIGKILL) | BIT(SIGSTOP));
    record("mask.unblockable_filtered", get_mask() == BIT(SIGUSR1), 1);
    set_mask(0);
    set_mask(BIT(SIGUSR1) | BIT(SIGUSR2));
    if (kill(getpid(), SIGUSR2)) fatal("queue SIGUSR2");
    uint64_t pending = 0;
    record("signal.pending_encoding", !syscall(SYS_rt_sigpending, &pending, 8) && pending == BIT(SIGUSR2), 1);
    mask = BIT(SIGUSR2);
    record("signal.timedwait_selects_bit", result(syscall(SYS_rt_sigtimedwait, &mask, NULL, &zero, 8)), SIGUSR2);
    record("signal.pending_consumed", !syscall(SYS_rt_sigpending, &pending, 8) && pending == 0, 1);
    mask = BIT(SIGUSR1);
    int sigfd = (int)syscall(SYS_signalfd4, -1, &mask, 8, SFD_NONBLOCK | SFD_CLOEXEC);
    if (sigfd < 0) fatal("signalfd");
    struct epoll_event sig_event = {.events=EPOLLIN, .data.u64=0x5151};
    if (epoll_ctl(ep, EPOLL_CTL_ADD, sigfd, &sig_event) || kill(getpid(), SIGUSR1)) fatal("signalfd queue");
    record("signal.signalfd_ready", wait2(ep, out, 1, &zero, NULL, 0) == 1 && out[0].data.u64 == 0x5151, 1);
    struct signalfd_siginfo info;
    record("signal.signalfd_number", read(sigfd, &info, sizeof(info)) == sizeof(info) && info.ssi_signo == SIGUSR1, 1);
    if (epoll_ctl(ep, EPOLL_CTL_DEL, sigfd, NULL) || close(sigfd)) fatal("signalfd cleanup");
    set_mask(0);
    timeout_case(ep, "timeout.one_ns", 1);
    timeout_case(ep, "timeout.sub_ms", 800000);
    timeout_case(ep, "timeout.one_ms", 1000000);
    timeout_case(ep, "timeout.seven_ms", 7000000);
    timeout_case(ep, "timeout.twenty_ms", 20000000);
    int evfd = eventfd(1, EFD_NONBLOCK); if (evfd < 0) fatal("eventfd");
    struct epoll_event interest = {.events=EPOLLIN, .data.u64=0x123456789abcdef0ULL};
    if (epoll_ctl(ep, EPOLL_CTL_ADD, evfd, &interest)) fatal("add eventfd");
    record("ready.value", wait2(ep, out, 1, &zero, NULL, 0) == 1 && out[0].events == EPOLLIN && out[0].data.u64 == interest.data.u64, 1);
    record("ready.level_repeats", wait2(ep, out, 1, &zero, NULL, 0), 1);
    record("ready.null_events", wait2(ep, NULL, 1, &zero, NULL, 0), -EFAULT);
    record("ready.unmapped_events", wait2(ep, (void *)1, 1, &zero, NULL, 0), -EFAULT);
    record("ready.fault_keeps_event", wait2(ep, out, 1, &zero, NULL, 0), 1);
    mask = BIT(SIGUSR2);
    set_mask(BIT(SIGUSR1));
    record("mask.copy_fault_restored", wait2(ep, (void *)1, 1, &zero, &mask, 8) == -EFAULT && get_mask() == BIT(SIGUSR1), 1);
    set_mask(0);
    uint64_t counter; if (read(evfd, &counter, 8) != 8 || counter != 1) fatal("eventfd read");
    record("ready.drain", wait2(ep, out, 1, &zero, NULL, 0), 0);
    if (epoll_ctl(ep, EPOLL_CTL_DEL, evfd, NULL) || close(evfd)) fatal("del eventfd");
    int regular = open("/etc/hosts", O_RDONLY);
    if (regular < 0) fatal("regular file");
    record("wait.wrong_descriptor", wait2(regular, out, 1, &zero, NULL, 0), -EINVAL);
    close(regular);
    int pipefd[2]; if (pipe(pipefd) || epoll_ctl(ep, EPOLL_CTL_ADD, pipefd[0], &interest)) fatal("pipe interest");
    delayed_readiness(ep, pipefd[0], pipefd[1], "timeout.null_infinite", NULL);
    struct timespec large = {.tv_sec=4294967, .tv_nsec=296000000};
    delayed_readiness(ep, pipefd[0], pipefd[1], "timeout.u32_milliseconds_wrap", &large);
    large = (struct timespec){.tv_sec=1LL<<60};
    delayed_readiness(ep, pipefd[0], pipefd[1], "timeout.seconds_multiply_overflow", &large);
    large = (struct timespec){.tv_sec=LONG_MAX, .tv_nsec=999999999};
    delayed_readiness(ep, pipefd[0], pipefd[1], "timeout.saturates", &large);
    interruption(ep, "pwait2.pending_interrupt", 2, 1);
    interruption(ep, "pwait2.sent_interrupt", 2, 0);
    interruption(ep, "pwait.sent_interrupt", 1, 0);
    interruption(ep, "wait.sent_interrupt", 0, 0);
    action.sa_flags |= SA_NODEFER; handler_self_block = 0;
    if (sigaction(SIGUSR1, &action, NULL)) fatal("nodefer handler");
    interruption(ep, "pwait2.nodefer", 2, 1);
    action.sa_flags &= ~SA_NODEFER; handler_self_block = 1;
    if (sigaction(SIGUSR1, &action, NULL)) fatal("restore handler");
    mask = BIT(SIGUSR1); signals = 0;
    pid_t child = sender(-1, SIGUSR1);
    struct timespec finite = {.tv_nsec=70000000};
    record("mask.block_during_wait", wait2(ep, out, 1, &finite, &mask, 8), 0);
    record("mask.deliver_after_timeout", signals, 1);
    record("mask.restore_after_timeout", get_mask() == 0, 1); reap(child);
    signals = 0; child = sender(pipefd[1], SIGUSR1);
    record("mask.ready_result", wait2(ep, out, 1, &large, &mask, 8), 1);
    record("mask.deliver_after_ready", signals, 1);
    record("mask.restore_after_ready", get_mask() == 0, 1); reap(child);
    char c; if (read(pipefd[0], &c, 1) != 1) fatal("drain ready");
    /* HUP is returned even when not explicitly requested. */
    interest.events = 0;
    if (epoll_ctl(ep, EPOLL_CTL_MOD, pipefd[0], &interest) || close(pipefd[1])) fatal("hup setup");
    record("ready.hup_without_interest", wait2(ep, out, 1, &zero, NULL, 0) == 1 && out[0].events == EPOLLHUP, 1);
    if (epoll_ctl(ep, EPOLL_CTL_DEL, pipefd[0], NULL) || close(pipefd[0]) || close(ep)) fatal("cleanup");

    /* Registration identity and modes */
    int ep_m = epoll_create1(0);
    if (ep_m < 0) fatal("epoll_create1 ep_m");

    struct epoll_event ev = {.events = EPOLLIN, .data.u64 = 42};

    /* 1. Validation & non-pollable files */
    int reg_file = open("/etc/hosts", O_RDONLY);
    if (reg_file < 0) fatal("open /etc/hosts");
    record("reg.file_eperm", result(epoll_ctl(ep_m, EPOLL_CTL_ADD, reg_file, &ev)), -EPERM);
    close(reg_file);

    int dir_fd = open("/tmp", O_RDONLY | O_DIRECTORY);
    if (dir_fd >= 0) {
        record("reg.dir_eperm", result(epoll_ctl(ep_m, EPOLL_CTL_ADD, dir_fd, &ev)), -EPERM);
        close(dir_fd);
    }

    record("reg.self_einval", result(epoll_ctl(ep_m, EPOLL_CTL_ADD, ep_m, &ev)), -EINVAL);
    record("reg.bad_fd", result(epoll_ctl(ep_m, EPOLL_CTL_ADD, -1, &ev)), -EBADF);

    int p[2];
    if (pipe(p) < 0) fatal("pipe");
    record("reg.unadded_mod_enoent", result(epoll_ctl(ep_m, EPOLL_CTL_MOD, p[0], &ev)), -ENOENT);
    record("reg.unadded_del_enoent", result(epoll_ctl(ep_m, EPOLL_CTL_DEL, p[0], NULL)), -ENOENT);

    record("reg.add_success", result(epoll_ctl(ep_m, EPOLL_CTL_ADD, p[0], &ev)), 0);
    record("reg.duplicate_eexist", result(epoll_ctl(ep_m, EPOLL_CTL_ADD, p[0], &ev)), -EEXIST);

    struct epoll_event excl_ev = {.events = EPOLLIN | EPOLLEXCLUSIVE, .data.u64 = 43};
    record("reg.exclusive_mod_einval", result(epoll_ctl(ep_m, EPOLL_CTL_MOD, p[0], &excl_ev)), -EINVAL);

    if (epoll_ctl(ep_m, EPOLL_CTL_DEL, p[0], NULL)) fatal("del p[0]");

    /* 2. Nested epoll & cycle detection */
    int ep2 = epoll_create1(0);
    if (ep2 < 0) fatal("epoll_create1 ep2");
    record("nest.add_ep2_to_ep", result(epoll_ctl(ep_m, EPOLL_CTL_ADD, ep2, &ev)), 0);
    record("nest.loop_eloop", result(epoll_ctl(ep2, EPOLL_CTL_ADD, ep_m, &ev)), -ELOOP);

    /* Test nested readiness: write to pipe in ep2 -> ep_m sees ep2 ready */
    if (epoll_ctl(ep2, EPOLL_CTL_ADD, p[0], &ev)) fatal("add p[0] to ep2");
    if (write(p[1], "n", 1) != 1) fatal("pipe write");
    record("nest.ready", wait2(ep_m, out, 1, &zero, NULL, 0) == 1 && out[0].data.u64 == 42, 1);
    char ch;
    if (read(p[0], &ch, 1) != 1) fatal("pipe read");
    close(ep2);

    /* 3. Multiple registrations via dup() and file description lifetime */
    int p_dup = dup(p[0]);
    if (p_dup < 0) fatal("dup");
    struct epoll_event ev_orig = {.events = EPOLLIN, .data.u64 = 100};
    struct epoll_event ev_dup  = {.events = EPOLLIN, .data.u64 = 200};
    record("reg.dup_add_orig", result(epoll_ctl(ep_m, EPOLL_CTL_ADD, p[0], &ev_orig)), 0);
    record("reg.dup_add_dup", result(epoll_ctl(ep_m, EPOLL_CTL_ADD, p_dup, &ev_dup)), 0);

    if (write(p[1], "d", 1) != 1) fatal("write dup test");
    long n_dup = wait2(ep_m, out, 4, &zero, NULL, 0);
    record("reg.dup_both_ready", n_dup == 2, 1);

    /* Close original fd: p_dup keeps the underlying file alive, so events still report */
    close(p[0]);
    long n_after_close = wait2(ep_m, out, 4, &zero, NULL, 0);
    record("reg.dup_after_close_orig", n_after_close == 2, 1);
    if (read(p_dup, &ch, 1) != 1) fatal("read p_dup");
    close(p_dup);

    /* 4. EPOLLET (edge-triggered) */
    int p_et[2];
    if (pipe(p_et) < 0) fatal("pipe et");
    struct epoll_event ev_et = {.events = EPOLLIN | EPOLLET, .data.u64 = 300};
    if (epoll_ctl(ep_m, EPOLL_CTL_ADD, p_et[0], &ev_et)) fatal("add et");
    if (write(p_et[1], "1", 1) != 1) fatal("write et 1");

    record("et.first_fire", wait2(ep_m, out, 1, &zero, NULL, 0) == 1 && out[0].data.u64 == 300, 1);
    /* Second wait without reading or new writing must be suppressed */
    record("et.second_wait_suppressed", wait2(ep_m, out, 1, &zero, NULL, 0), 0);

    /* Writing new byte triggers a new edge */
    if (write(p_et[1], "2", 1) != 1) fatal("write et 2");
    record("et.new_write_fires", wait2(ep_m, out, 1, &zero, NULL, 0) == 1 && out[0].data.u64 == 300, 1);
    record("et.third_wait_suppressed", wait2(ep_m, out, 1, &zero, NULL, 0), 0);

    /* Drain pipe */
    char buf[4];
    if (read(p_et[0], buf, 2) != 2) fatal("read et");
    if (epoll_ctl(ep_m, EPOLL_CTL_DEL, p_et[0], NULL)) fatal("del et");
    close(p_et[0]); close(p_et[1]);

    /* 5. EPOLLONESHOT */
    int p_os[2];
    if (pipe(p_os) < 0) fatal("pipe os");
    struct epoll_event ev_os = {.events = EPOLLIN | EPOLLONESHOT, .data.u64 = 400};
    if (epoll_ctl(ep_m, EPOLL_CTL_ADD, p_os[0], &ev_os)) fatal("add os");
    if (write(p_os[1], "a", 1) != 1) fatal("write os 1");

    record("oneshot.first_fire", wait2(ep_m, out, 1, &zero, NULL, 0) == 1 && out[0].data.u64 == 400, 1);
    record("oneshot.second_suppressed", wait2(ep_m, out, 1, &zero, NULL, 0), 0);
    if (write(p_os[1], "b", 1) != 1) fatal("write os 2");
    record("oneshot.new_write_still_suppressed", wait2(ep_m, out, 1, &zero, NULL, 0), 0);

    /* MOD rearms oneshot */
    record("oneshot.mod_rearm", result(epoll_ctl(ep_m, EPOLL_CTL_MOD, p_os[0], &ev_os)), 0);
    record("oneshot.rearmed_fires", wait2(ep_m, out, 1, &zero, NULL, 0) == 1 && out[0].data.u64 == 400, 1);

    if (epoll_ctl(ep_m, EPOLL_CTL_DEL, p_os[0], NULL)) fatal("del os");
    close(p_os[0]); close(p_os[1]);
    close(p[1]);
    close(ep_m);

    race_cases();
    set_mask(0); alarm(0);
    puts("== namespaces skipped: 0 ==");
    printf("== probe complete: %d passed, %d failed ==\n", passed, failed);
    return failed != 0;
}
