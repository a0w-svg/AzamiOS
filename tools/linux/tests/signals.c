#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define KERNEL_NSIG_BYTES 8

static int passed, failed;

static void record(const char *name, long value, long expected)
{
    int good = (value == expected);
    printf("NSCASE {\"name\":\"%s\",\"value\":%ld,\"expected\":%ld,\"passed\":%s}\n",
           name, value, expected, good ? "true" : "false");
    if (good) passed++; else failed++;
}

static pid_t get_tid(void)
{
    return (pid_t)syscall(SYS_gettid);
}

static long do_tkill(pid_t tid, int sig)
{
    long ret = syscall(SYS_tkill, tid, sig);
    return ret < 0 ? -errno : ret;
}

static long do_tgkill(pid_t tgid, pid_t tid, int sig)
{
    long ret = syscall(SYS_tgkill, tgid, tid, sig);
    return ret < 0 ? -errno : ret;
}

static long do_rt_sigprocmask(int how, const sigset_t *set, sigset_t *oldset, size_t sigsetsize)
{
    long ret = syscall(SYS_rt_sigprocmask, how, set, oldset, sigsetsize);
    return ret < 0 ? -errno : ret;
}

static long do_rt_sigpending(sigset_t *set, size_t sigsetsize)
{
    long ret = syscall(SYS_rt_sigpending, set, sigsetsize);
    return ret < 0 ? -errno : ret;
}

static long do_sigaltstack(const stack_t *ss, stack_t *old_ss)
{
    long ret = syscall(SYS_sigaltstack, ss, old_ss);
    return ret < 0 ? -errno : ret;
}

static long do_rt_sigsuspend(const sigset_t *mask, size_t sigsetsize)
{
    long ret = syscall(SYS_rt_sigsuspend, mask, sigsetsize);
    return ret < 0 ? -errno : ret;
}

static long do_rt_sigtimedwait(const sigset_t *set, siginfo_t *info,
                               const struct timespec *timeout, size_t sigsetsize)
{
    long ret = syscall(SYS_rt_sigtimedwait, set, info, timeout, sigsetsize);
    return ret < 0 ? -errno : ret;
}

/* Global handler tracking */
static volatile sig_atomic_t g_handler_ran = 0;
static volatile sig_atomic_t g_handler_tid = 0;
static volatile sig_atomic_t g_handler_on_altstack = 0;
static volatile sig_atomic_t g_handler_altstack_flags = 0;
static volatile sig_atomic_t g_handler_altstack_mod_err = 0;
static char g_altstack_buf[16384];

static void sigusr1_handler(int sig)
{
    (void)sig;
    g_handler_ran = 1;
    g_handler_tid = get_tid();
}

static void sigusr1_altstack_handler(int sig)
{
    (void)sig;
    g_handler_ran = 1;
    g_handler_tid = get_tid();

    char marker = 0;
    uintptr_t sp = (uintptr_t)&marker;
    uintptr_t base = (uintptr_t)g_altstack_buf;
    if (sp >= base && sp < base + sizeof(g_altstack_buf)) {
        g_handler_on_altstack = 1;
    }

    stack_t cur_st;
    memset(&cur_st, 0, sizeof(cur_st));
    if (do_sigaltstack(NULL, &cur_st) == 0) {
        g_handler_altstack_flags = cur_st.ss_flags;
    }

    /* Modifying alternate stack while executing on it must fail with -EPERM */
    stack_t new_st;
    new_st.ss_sp = g_altstack_buf;
    new_st.ss_size = sizeof(g_altstack_buf);
    new_st.ss_flags = 0;
    g_handler_altstack_mod_err = do_sigaltstack(&new_st, NULL);
}

/* Thread sync helpers */
struct thread_arg {
    pthread_barrier_t barrier;
    pid_t tid;
    volatile int stop;
    volatile int ready;
};

static void *mask_test_worker(void *arg)
{
    struct thread_arg *ta = (struct thread_arg *)arg;
    ta->tid = get_tid();

    /* Worker blocks SIGUSR2 */
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR2);
    do_rt_sigprocmask(SIG_BLOCK, &set, NULL, KERNEL_NSIG_BYTES);

    pthread_barrier_wait(&ta->barrier);
    pthread_barrier_wait(&ta->barrier);

    /* Verify mask: worker still has SIGUSR2 blocked and SIGUSR1 unblocked */
    sigset_t cur;
    sigemptyset(&cur);
    do_rt_sigprocmask(SIG_BLOCK, NULL, &cur, KERNEL_NSIG_BYTES);

    int b_sigusr2 = sigismember(&cur, SIGUSR2);
    int b_sigusr1 = sigismember(&cur, SIGUSR1);
    record("procmask.worker_has_sigusr2", b_sigusr2, 1);
    record("procmask.worker_lacks_sigusr1", b_sigusr1, 0);

    return NULL;
}

static void *delivery_target_worker(void *arg)
{
    struct thread_arg *ta = (struct thread_arg *)arg;
    ta->tid = get_tid();
    ta->ready = 1;

    while (!ta->stop) {
        struct timespec ts = {0, 1000000}; /* 1 ms */
        nanosleep(&ts, NULL);
    }
    return NULL;
}

static void *pending_test_worker(void *arg)
{
    struct thread_arg *ta = (struct thread_arg *)arg;
    ta->tid = get_tid();

    /* Worker blocks SIGUSR1 */
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    do_rt_sigprocmask(SIG_BLOCK, &set, NULL, KERNEL_NSIG_BYTES);

    pthread_barrier_wait(&ta->barrier);
    /* Main thread will now tgkill SIGUSR1 to worker */
    pthread_barrier_wait(&ta->barrier);

    /* Worker checks sigpending: SIGUSR1 must be pending */
    sigset_t pend;
    sigemptyset(&pend);
    do_rt_sigpending(&pend, KERNEL_NSIG_BYTES);
    record("queue.worker_sees_pending", sigismember(&pend, SIGUSR1), 1);

    /* Worker unblocks SIGUSR1: handler must run immediately */
    g_handler_ran = 0;
    g_handler_tid = 0;
    do_rt_sigprocmask(SIG_UNBLOCK, &set, NULL, KERNEL_NSIG_BYTES);
    record("queue.worker_handled_on_unblock", g_handler_ran, 1);
    record("queue.worker_handled_tid_matches", g_handler_tid == ta->tid, 1);

    /* Worker checks sigpending again: SIGUSR1 must now be cleared */
    sigemptyset(&pend);
    do_rt_sigpending(&pend, KERNEL_NSIG_BYTES);
    record("queue.worker_pending_cleared", sigismember(&pend, SIGUSR1), 0);

    return NULL;
}

static void *shared_pending_worker(void *arg)
{
    struct thread_arg *ta = (struct thread_arg *)arg;
    ta->tid = get_tid();

    /* Both threads block SIGUSR2 */
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR2);
    do_rt_sigprocmask(SIG_BLOCK, &set, NULL, KERNEL_NSIG_BYTES);

    pthread_barrier_wait(&ta->barrier);
    /* Main thread kills process with SIGUSR2 */
    pthread_barrier_wait(&ta->barrier);

    /* Worker checks pending: must see SIGUSR2 */
    sigset_t pend;
    sigemptyset(&pend);
    do_rt_sigpending(&pend, KERNEL_NSIG_BYTES);
    record("shared.worker_sees_proc_pending", sigismember(&pend, SIGUSR2), 1);

    /* Worker unblocks SIGUSR2 and delivers it */
    g_handler_ran = 0;
    do_rt_sigprocmask(SIG_UNBLOCK, &set, NULL, KERNEL_NSIG_BYTES);
    record("shared.worker_handled_proc_sig", g_handler_ran, 1);

    pthread_barrier_wait(&ta->barrier);
    return NULL;
}

static void *altstack_worker(void *arg)
{
    struct thread_arg *ta = (struct thread_arg *)arg;
    ta->tid = get_tid();

    /* Worker queries sigaltstack: must be SS_DISABLE */
    stack_t st;
    memset(&st, 0, sizeof(st));
    long r = do_sigaltstack(NULL, &st);
    record("altstack.worker_query_ret", r, 0);
    record("altstack.worker_flags_disabled", (st.ss_flags & SS_DISABLE) ? 1 : 0, 1);

    pthread_barrier_wait(&ta->barrier);
    return NULL;
}

static void *inherited_mask_worker(void *arg)
{
    (void)arg;
    /* Check mask inherited from main thread: SIGUSR1 must be blocked */
    sigset_t cur;
    sigemptyset(&cur);
    do_rt_sigprocmask(SIG_BLOCK, NULL, &cur, KERNEL_NSIG_BYTES);
    record("inherit.mask_inherited", sigismember(&cur, SIGUSR1), 1);

    /* Pending signals must NOT be inherited */
    sigset_t pend;
    sigemptyset(&pend);
    do_rt_sigpending(&pend, KERNEL_NSIG_BYTES);
    record("inherit.pending_not_inherited", sigismember(&pend, SIGUSR1), 0);

    /* Alternate stack must NOT be inherited */
    stack_t st;
    memset(&st, 0, sizeof(st));
    do_sigaltstack(NULL, &st);
    record("inherit.altstack_disabled", (st.ss_flags & SS_DISABLE) ? 1 : 0, 1);

    return NULL;
}

static void *sigsuspend_sender(void *arg)
{
    pid_t target_tid = *(pid_t *)arg;
    struct timespec ts = {0, 20000000}; /* 20 ms */
    nanosleep(&ts, NULL);
    do_tgkill(getpid(), target_tid, SIGUSR1);
    return NULL;
}

int main(void)
{
    pid_t pid = getpid();
    pid_t main_tid = get_tid();

    /* 1. Basic test */
    record("signals.basic", 1, 1);

    /* 2. rt_sigprocmask validation */
    sigset_t dummy_set;
    sigemptyset(&dummy_set);
    record("procmask.invalid_how", do_rt_sigprocmask(999, &dummy_set, NULL, KERNEL_NSIG_BYTES), -EINVAL);
    record("procmask.invalid_size_zero", do_rt_sigprocmask(SIG_BLOCK, NULL, NULL, 0), -EINVAL);
    record("procmask.invalid_size_large", do_rt_sigprocmask(SIG_BLOCK, NULL, NULL, 16), -EINVAL);
    record("procmask.bad_pointer", do_rt_sigprocmask(SIG_BLOCK, (const sigset_t *)0x1, NULL, KERNEL_NSIG_BYTES), -EFAULT);

    /* Null set queries oldmask without modifying */
    sigset_t orig_mask;
    sigemptyset(&orig_mask);
    record("procmask.null_set_query", do_rt_sigprocmask(SIG_BLOCK, NULL, &orig_mask, KERNEL_NSIG_BYTES), 0);

    /* Unblockable signals cannot be blocked */
    sigset_t attempt;
    sigemptyset(&attempt);
    sigaddset(&attempt, SIGKILL);
    sigaddset(&attempt, SIGSTOP);
    sigaddset(&attempt, SIGUSR1);
    do_rt_sigprocmask(SIG_BLOCK, &attempt, NULL, KERNEL_NSIG_BYTES);

    sigset_t test_mask;
    sigemptyset(&test_mask);
    do_rt_sigprocmask(SIG_BLOCK, NULL, &test_mask, KERNEL_NSIG_BYTES);
    record("procmask.usr1_blocked", sigismember(&test_mask, SIGUSR1), 1);
    record("procmask.kill_not_blocked", sigismember(&test_mask, SIGKILL), 0);
    record("procmask.stop_not_blocked", sigismember(&test_mask, SIGSTOP), 0);

    /* Restore clean mask */
    do_rt_sigprocmask(SIG_SETMASK, &orig_mask, NULL, KERNEL_NSIG_BYTES);

    /* 3. Thread-independence of sigprocmask */
    struct thread_arg ta_mask;
    pthread_barrier_init(&ta_mask.barrier, NULL, 2);
    pthread_t th_mask;
    pthread_create(&th_mask, NULL, mask_test_worker, &ta_mask);

    /* Main thread blocks SIGUSR1 */
    sigset_t main_block;
    sigemptyset(&main_block);
    sigaddset(&main_block, SIGUSR1);
    do_rt_sigprocmask(SIG_BLOCK, &main_block, NULL, KERNEL_NSIG_BYTES);

    pthread_barrier_wait(&ta_mask.barrier);
    pthread_barrier_wait(&ta_mask.barrier);

    /* Check main thread's mask: has SIGUSR1 blocked and SIGUSR2 unblocked */
    sigset_t main_cur;
    sigemptyset(&main_cur);
    do_rt_sigprocmask(SIG_BLOCK, NULL, &main_cur, KERNEL_NSIG_BYTES);
    record("procmask.main_has_sigusr1", sigismember(&main_cur, SIGUSR1), 1);
    record("procmask.main_lacks_sigusr2", sigismember(&main_cur, SIGUSR2), 0);

    pthread_join(th_mask, NULL);
    pthread_barrier_destroy(&ta_mask.barrier);
    do_rt_sigprocmask(SIG_SETMASK, &orig_mask, NULL, KERNEL_NSIG_BYTES);

    /* 4. tkill / tgkill argument validation */
    record("tkill.invalid_tid_zero", do_tkill(0, SIGUSR1), -EINVAL);
    record("tkill.invalid_tid_negative", do_tkill(-1, SIGUSR1), -EINVAL);
    record("tkill.invalid_sig_negative", do_tkill(main_tid, -1), -EINVAL);
    record("tkill.invalid_sig_overflow", do_tkill(main_tid, 65), -EINVAL);
    record("tkill.esrch_dead_tid", do_tkill(999999, SIGUSR1), -ESRCH);
    record("tkill.probe_sig0", do_tkill(main_tid, 0), 0);

    record("tgkill.invalid_tgid_zero", do_tgkill(0, main_tid, SIGUSR1), -EINVAL);
    record("tgkill.invalid_tgid_negative", do_tgkill(-1, main_tid, SIGUSR1), -EINVAL);
    record("tgkill.invalid_tid_zero", do_tgkill(pid, 0, SIGUSR1), -EINVAL);
    record("tgkill.invalid_tid_negative", do_tgkill(pid, -1, SIGUSR1), -EINVAL);
    record("tgkill.invalid_sig_negative", do_tgkill(pid, main_tid, -1), -EINVAL);
    record("tgkill.invalid_sig_overflow", do_tgkill(pid, main_tid, 65), -EINVAL);
    record("tgkill.mismatched_tgid", do_tgkill(pid + 100, main_tid, SIGUSR1), -ESRCH);
    record("tgkill.esrch_dead_tid", do_tgkill(pid, 999999, SIGUSR1), -ESRCH);
    record("tgkill.probe_sig0", do_tgkill(pid, main_tid, 0), 0);

    /* 5. Directed delivery to specific thread via tkill / tgkill */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigusr1_handler;
    sigaction(SIGUSR1, &sa, NULL);

    struct thread_arg ta_target;
    ta_target.ready = 0;
    ta_target.stop = 0;
    pthread_t th_target;
    pthread_create(&th_target, NULL, delivery_target_worker, &ta_target);
    while (!ta_target.ready) {
        struct timespec ts = {0, 1000000};
        nanosleep(&ts, NULL);
    }

    /* Send tgkill to worker thread */
    g_handler_ran = 0;
    g_handler_tid = 0;
    record("deliver.tgkill_worker_ret", do_tgkill(pid, ta_target.tid, SIGUSR1), 0);
    for (int i = 0; i < 50 && !g_handler_ran; i++) {
        struct timespec ts = {0, 2000000};
        nanosleep(&ts, NULL);
    }
    record("deliver.tgkill_worker_ran", g_handler_ran, 1);
    record("deliver.tgkill_worker_tid_match", g_handler_tid == ta_target.tid, 1);

    /* Send tkill to main thread */
    g_handler_ran = 0;
    g_handler_tid = 0;
    record("deliver.tkill_main_ret", do_tkill(main_tid, SIGUSR1), 0);
    record("deliver.tkill_main_ran", g_handler_ran, 1);
    record("deliver.tkill_main_tid_match", g_handler_tid == main_tid, 1);

    ta_target.stop = 1;
    pthread_join(th_target, NULL);

    /* 6. Directed signal queuing on blocked threads */
    struct thread_arg ta_pend;
    pthread_barrier_init(&ta_pend.barrier, NULL, 2);
    pthread_t th_pend;
    pthread_create(&th_pend, NULL, pending_test_worker, &ta_pend);

    pthread_barrier_wait(&ta_pend.barrier);
    /* Worker is now blocking SIGUSR1. Send SIGUSR1 specifically to worker */
    do_tgkill(pid, ta_pend.tid, SIGUSR1);

    /* Main thread checks sigpending: main thread must NOT see SIGUSR1 pending */
    sigset_t main_pend;
    sigemptyset(&main_pend);
    do_rt_sigpending(&main_pend, KERNEL_NSIG_BYTES);
    record("queue.main_lacks_pending_worker_sig", sigismember(&main_pend, SIGUSR1), 0);

    pthread_barrier_wait(&ta_pend.barrier);
    pthread_join(th_pend, NULL);
    pthread_barrier_destroy(&ta_pend.barrier);

    /* 7. Process-directed signal queuing (kill(pid, SIGUSR2)) */
    struct sigaction sa2;
    memset(&sa2, 0, sizeof(sa2));
    sa2.sa_handler = sigusr1_handler;
    sigaction(SIGUSR2, &sa2, NULL);

    struct thread_arg ta_shared;
    pthread_barrier_init(&ta_shared.barrier, NULL, 2);
    pthread_t th_shared;
    pthread_create(&th_shared, NULL, shared_pending_worker, &ta_shared);

    /* Main thread blocks SIGUSR2 */
    sigset_t block_u2;
    sigemptyset(&block_u2);
    sigaddset(&block_u2, SIGUSR2);
    do_rt_sigprocmask(SIG_BLOCK, &block_u2, NULL, KERNEL_NSIG_BYTES);

    pthread_barrier_wait(&ta_shared.barrier);
    /* Send process-wide signal */
    kill(pid, SIGUSR2);

    /* Main thread checks pending: must see SIGUSR2 */
    sigemptyset(&main_pend);
    do_rt_sigpending(&main_pend, KERNEL_NSIG_BYTES);
    record("shared.main_sees_proc_pending", sigismember(&main_pend, SIGUSR2), 1);

    pthread_barrier_wait(&ta_shared.barrier);
    /* Worker unblocked and handled it */
    pthread_barrier_wait(&ta_shared.barrier);

    /* Now that worker handled it, main thread checks pending: should be consumed */
    sigemptyset(&main_pend);
    do_rt_sigpending(&main_pend, KERNEL_NSIG_BYTES);
    record("shared.proc_pending_cleared_after_handle", sigismember(&main_pend, SIGUSR2), 0);

    pthread_join(th_shared, NULL);
    pthread_barrier_destroy(&ta_shared.barrier);
    do_rt_sigprocmask(SIG_SETMASK, &orig_mask, NULL, KERNEL_NSIG_BYTES);

    /* 8. sigaltstack per-thread and execution */
    record("altstack.invalid_size", ({
        stack_t small_st = {.ss_sp = g_altstack_buf, .ss_size = 512, .ss_flags = 0};
        do_sigaltstack(&small_st, NULL);
    }), -ENOMEM);

    record("altstack.invalid_flags", ({
        stack_t bad_flags = {.ss_sp = g_altstack_buf, .ss_size = sizeof(g_altstack_buf), .ss_flags = 4};
        do_sigaltstack(&bad_flags, NULL);
    }), -EINVAL);

    record("altstack.bad_pointer", do_sigaltstack((const stack_t *)0x1, NULL), -EFAULT);

    /* Set alternate stack on main thread */
    stack_t main_st = {
        .ss_sp = g_altstack_buf,
        .ss_size = sizeof(g_altstack_buf),
        .ss_flags = 0
    };
    record("altstack.main_install", do_sigaltstack(&main_st, NULL), 0);

    stack_t old_st;
    memset(&old_st, 0, sizeof(old_st));
    record("altstack.main_query", do_sigaltstack(NULL, &old_st), 0);
    record("altstack.main_flags_enabled", old_st.ss_flags, 0);
    record("altstack.main_sp_match", (uintptr_t)old_st.ss_sp == (uintptr_t)g_altstack_buf, 1);

    /* Check that a worker thread does NOT inherit or share this alternate stack */
    struct thread_arg ta_alt;
    pthread_barrier_init(&ta_alt.barrier, NULL, 2);
    pthread_t th_alt;
    pthread_create(&th_alt, NULL, altstack_worker, &ta_alt);
    pthread_barrier_wait(&ta_alt.barrier);
    pthread_join(th_alt, NULL);
    pthread_barrier_destroy(&ta_alt.barrier);

    /* Install handler with SA_ONSTACK and verify delivery onto altstack */
    struct sigaction sa_alt;
    memset(&sa_alt, 0, sizeof(sa_alt));
    sa_alt.sa_handler = sigusr1_altstack_handler;
    sa_alt.sa_flags = SA_ONSTACK;
    sigaction(SIGUSR1, &sa_alt, NULL);

    g_handler_ran = 0;
    g_handler_on_altstack = 0;
    g_handler_altstack_flags = 0;
    g_handler_altstack_mod_err = 0;
    do_tkill(main_tid, SIGUSR1);

    record("altstack.handler_ran", g_handler_ran, 1);
    record("altstack.delivered_on_altstack", g_handler_on_altstack, 1);
    record("altstack.flags_had_onstack", (g_handler_altstack_flags & SS_ONSTACK) ? 1 : 0, 1);
    record("altstack.modify_while_active_eperm", g_handler_altstack_mod_err, -EPERM);

    /* Disable alternate stack */
    stack_t dis_st = {.ss_sp = NULL, .ss_size = 0, .ss_flags = SS_DISABLE};
    record("altstack.disable_ret", do_sigaltstack(&dis_st, NULL), 0);
    memset(&old_st, 0, sizeof(old_st));
    do_sigaltstack(NULL, &old_st);
    record("altstack.disabled_flag", (old_st.ss_flags & SS_DISABLE) ? 1 : 0, 1);

    /* 9. Inheritance across pthread_create */
    sigset_t inh_mask;
    sigemptyset(&inh_mask);
    sigaddset(&inh_mask, SIGUSR1);
    do_rt_sigprocmask(SIG_BLOCK, &inh_mask, NULL, KERNEL_NSIG_BYTES);

    pthread_t th_inh;
    pthread_create(&th_inh, NULL, inherited_mask_worker, NULL);
    pthread_join(th_inh, NULL);
    do_rt_sigprocmask(SIG_SETMASK, &orig_mask, NULL, KERNEL_NSIG_BYTES);

    /* 10. sigsuspend */
    sa.sa_handler = sigusr1_handler;
    sa.sa_flags = 0;
    sigaction(SIGUSR1, &sa, NULL);

    sigset_t susp_block;
    sigemptyset(&susp_block);
    sigaddset(&susp_block, SIGUSR1);
    do_rt_sigprocmask(SIG_BLOCK, &susp_block, NULL, KERNEL_NSIG_BYTES);

    g_handler_ran = 0;
    pthread_t th_sender;
    pthread_create(&th_sender, NULL, sigsuspend_sender, &main_tid);

    sigset_t susp_mask;
    sigemptyset(&susp_mask);
    record("sigsuspend.returns_eintr", do_rt_sigsuspend(&susp_mask, KERNEL_NSIG_BYTES), -EINTR);
    record("sigsuspend.handler_ran", g_handler_ran, 1);

    pthread_join(th_sender, NULL);

    /* Verify original mask was restored after sigsuspend */
    sigset_t post_susp;
    sigemptyset(&post_susp);
    do_rt_sigprocmask(SIG_BLOCK, NULL, &post_susp, KERNEL_NSIG_BYTES);
    record("sigsuspend.mask_restored", sigismember(&post_susp, SIGUSR1), 1);
    do_rt_sigprocmask(SIG_SETMASK, &orig_mask, NULL, KERNEL_NSIG_BYTES);

    /* 11. rt_sigtimedwait */
    sigset_t tw_mask;
    sigemptyset(&tw_mask);
    sigaddset(&tw_mask, SIGUSR1);
    do_rt_sigprocmask(SIG_BLOCK, &tw_mask, NULL, KERNEL_NSIG_BYTES);

    /* Queue SIGUSR1 via tkill */
    do_tkill(main_tid, SIGUSR1);

    /* Consume via sigtimedwait */
    siginfo_t si;
    memset(&si, 0, sizeof(si));
    struct timespec zts = {0, 0};
    long tw_ret = do_rt_sigtimedwait(&tw_mask, &si, &zts, KERNEL_NSIG_BYTES);
    record("timedwait.consumed_sigusr1", tw_ret, SIGUSR1);
    record("timedwait.siginfo_signo", si.si_signo, SIGUSR1);

    /* Verify signal is no longer pending */
    sigset_t post_tw_pend;
    sigemptyset(&post_tw_pend);
    do_rt_sigpending(&post_tw_pend, KERNEL_NSIG_BYTES);
    record("timedwait.not_pending_after_consume", sigismember(&post_tw_pend, SIGUSR1), 0);

    /* sigtimedwait with nothing pending returns -EAGAIN */
    record("timedwait.empty_eagain", do_rt_sigtimedwait(&tw_mask, &si, &zts, KERNEL_NSIG_BYTES), -EAGAIN);
    do_rt_sigprocmask(SIG_SETMASK, &orig_mask, NULL, KERNEL_NSIG_BYTES);

    /* Summary */
    printf("== namespaces skipped: 0 ==\n");
    printf("== probe complete: %d passed, %d failed ==\n", passed, failed);

    return failed > 0 ? 1 : 0;
}
