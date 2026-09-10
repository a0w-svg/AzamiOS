/* ============================================================================
 * AzamiOS Userspace — POSIX Signals Implementation (signal.c)
 * File: userland/libc/signal.c
 * ============================================================================ */

#include "include/signal.h"
#include "include/sys/syscall.h"
#include "include/unistd.h"
#include "include/errno.h"

/* Signal return trampoline. The kernel points the handler's return address here;
 * it invokes rt_sigreturn(2), which restores the interrupted context. */
__asm__(
    ".globl __az_sigreturn\n"
    ".type  __az_sigreturn,@function\n"
    "__az_sigreturn:\n"
    "    movl $15, %eax\n"      /* SYS_rt_sigreturn */
    "    syscall\n"
    "    hlt\n"                 /* never reached */
);
extern void __az_sigreturn(void);

/* AzamiOS sigset_t convention: bit N represents signal N (bit 0 unused). This
 * matches the kernel (process_t::sig_pending / sig_blocked use 1<<sig). */

int sigemptyset(sigset_t *set)
{
    if (!set) return -1;
    *set = 0;
    return 0;
}

int sigfillset(sigset_t *set)
{
    if (!set) return -1;
    *set = ~0UL;
    return 0;
}

int sigaddset(sigset_t *set, int signum)
{
    if (!set || signum < 1 || signum > 63) return -1;
    *set |= (1UL << signum);
    return 0;
}

int sigdelset(sigset_t *set, int signum)
{
    if (!set || signum < 1 || signum > 63) return -1;
    *set &= ~(1UL << signum);
    return 0;
}

int sigismember(const sigset_t *set, int signum)
{
    if (!set || signum < 1 || signum > 63) return -1;
    return (*set & (1UL << signum)) ? 1 : 0;
}

int raise(int sig)
{
    return kill(getpid(), sig);
}

int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact)
{
    struct sigaction tmp;
    if (act) {
        tmp = *act;
        if (!tmp.sa_restorer) {
            tmp.sa_restorer = (void (*)(void))__az_sigreturn;
            tmp.sa_flags |= SA_RESTORER;
        }
        act = &tmp;
    }
    return (int)syscall4(SYS_rt_sigaction, signum, (long)act, (long)oldact, 8 /* sizeof(sigset_t) */);
}

int sigprocmask(int how, const sigset_t *set, sigset_t *oldset)
{
    return (int)syscall4(SYS_rt_sigprocmask, how, (long)set, (long)oldset, 8 /* sizeof(sigset_t) */);
}

sighandler_t signal(int signum, sighandler_t handler)
{
    struct sigaction sa, old_sa;
    sa.sa_handler = handler;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sa.sa_restorer = 0;

    if (sigaction(signum, &sa, &old_sa) < 0) {
        return SIG_ERR;
    }
    return old_sa.sa_handler;
}

#include "include/stdio.h"
#include "include/string.h"

int sigaltstack(const stack_t *ss, stack_t *old_ss)
{
    long r = syscall2(SYS_sigaltstack, (long)ss, (long)old_ss);
    if (r < 0 && r > -4096) {
        errno = (int)-r;
        return -1;
    }
    return 0;
}

int pthread_sigmask(int how, const sigset_t *set, sigset_t *oldset)
{
    long r = syscall4(SYS_rt_sigprocmask, how, (long)set, (long)oldset, sizeof(sigset_t));
    if (r < 0 && r > -4096) {
        return (int)-r;
    }
    return 0;
}

int sigisemptyset(const sigset_t *set)
{
    if (!set) return 0;
    return *set == 0;
}

int sigorset(sigset_t *dest, const sigset_t *left, const sigset_t *right)
{
    if (!dest || !left || !right) return -1;
    *dest = *left | *right;
    return 0;
}

int sigandset(sigset_t *dest, const sigset_t *left, const sigset_t *right)
{
    if (!dest || !left || !right) return -1;
    *dest = *left & *right;
    return 0;
}

int sigpending(sigset_t *set)
{
    if (!set) { errno = EFAULT; return -1; }
    long r = syscall2(SYS_rt_sigpending, (long)set, sizeof(sigset_t));
    if (r < 0 && r > -4096) {
        errno = (int)-r;
        return -1;
    }
    return 0;
}

int sigsuspend(const sigset_t *mask)
{
    /* Always returns -1 with EINTR once a signal arrives; there is no
     * success case. */
    long r = syscall2(SYS_rt_sigsuspend, (long)mask, (long)sizeof(sigset_t));
    if (r < 0 && r > -4096) { errno = (int)-r; return -1; }
    return -1;
}

int sigwait(const sigset_t *set, int *sig)
{
    /* Accept a signal from @set rather than letting it run a handler.
     * Unlike the rest of the API, the error is the return value. */
    siginfo_t info;
    int r = sigwaitinfo(set, &info);
    if (r < 0) return errno;
    if (sig) *sig = info.si_signo;
    return 0;
}

int siginterrupt(int sig, int flag)
{
    struct sigaction act;
    if (sigaction(sig, NULL, &act) < 0) return -1;
    if (flag) act.sa_flags &= ~SA_RESTART;
    else act.sa_flags |= SA_RESTART;
    return sigaction(sig, &act, NULL);
}

int sighold(int sig)
{
    sigset_t set = 0;
    sigaddset(&set, sig);
    return sigprocmask(SIG_BLOCK, &set, NULL);
}

int sigrelse(int sig)
{
    sigset_t set = 0;
    sigaddset(&set, sig);
    return sigprocmask(SIG_UNBLOCK, &set, NULL);
}

int sigignore(int sig)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_IGN;
    return sigaction(sig, &sa, NULL);
}

int sigpause(int sig)
{
    sigset_t set;
    if (sigprocmask(SIG_SETMASK, NULL, &set) < 0) return -1;
    sigdelset(&set, sig);
    return sigsuspend(&set);
}


void psiginfo(const siginfo_t *pinfo, const char *s)
{
    if (!pinfo) {
        psignal(0, s);
        return;
    }
    psignal(pinfo->si_signo, s);
}
