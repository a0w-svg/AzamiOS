/* ============================================================================
 * AzamiOS Userspace — POSIX Signals Implementation (signal.c)
 * File: userland/libc/signal.c
 * ============================================================================ */

#include "include/signal.h"
#include "include/sys/syscall.h"
#include "include/unistd.h"

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
    if (!set || signum < 1 || signum > 64) return -1;
    *set |= (1UL << (signum - 1));
    return 0;
}

int sigdelset(sigset_t *set, int signum)
{
    if (!set || signum < 1 || signum > 64) return -1;
    *set &= ~(1UL << (signum - 1));
    return 0;
}

int sigismember(const sigset_t *set, int signum)
{
    if (!set || signum < 1 || signum > 64) return -1;
    return (*set & (1UL << (signum - 1))) ? 1 : 0;
}

int raise(int sig)
{
    return kill(getpid(), sig);
}

int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact)
{
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

int sigpending(sigset_t *set)
{
    if (!set) return -1;
    *set = 0;
    return 0;
}

int sigsuspend(const sigset_t *mask)
{
    (void)mask;
    return pause();
}

int sigwait(const sigset_t *set, int *sig)
{
    (void)set;
    if (sig) *sig = 0;
    pause();
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
