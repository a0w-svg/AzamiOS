/* ============================================================================
 * AzamiOS Userspace — POSIX Signals Header (signal.h)
 * File: userland/libc/include/signal.h
 * ============================================================================ */
#pragma once

#include "sys/types.h"

typedef unsigned long sigset_t;

/* Standard POSIX Signals */
#define SIGHUP    1
#define SIGINT    2
#define SIGQUIT   3
#define SIGILL    4
#define SIGTRAP   5
#define SIGABRT   6
#define SIGBUS    7
#define SIGFPE    8
#define SIGKILL   9
#define SIGUSR1  10
#define SIGSEGV  11
#define SIGUSR2  12
#define SIGPIPE  13
#define SIGALRM  14
#define SIGTERM  15
#define SIGSTKFLT 16
#define SIGCHLD  17
#define SIGCONT  18
#define SIGSTOP  19
#define SIGTSTP  20
#define SIGTTIN  21
#define SIGTTOU  22
#define SIGURG   23
#define SIGXCPU  24
#define SIGXFSZ  25
#define SIGVTALRM 26
#define SIGPROF  27
#define SIGWINCH 28
#define SIGIO    29
#define SIGPOLL  SIGIO
#define SIGPWR   30
#define SIGSYS   31
#define _NSIG    64

typedef void (*sighandler_t)(int);

#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)
#define SIG_ERR ((sighandler_t)-1)

/* Sigaction flags */
#define SA_NOCLDSTOP 0x00000001
#define SA_NOCLDWAIT 0x00000002
#define SA_SIGINFO   0x00000004
#define SA_RESTORER  0x04000000
#define SA_ONSTACK   0x08000000
#define SA_RESTART   0x10000000
#define SA_NODEFER   0x40000000
#define SA_RESETHAND 0x80000000

/* Sigprocmask how */
#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

struct sigaction {
    sighandler_t sa_handler;
    unsigned long sa_flags;
    void (*sa_restorer)(void);
    sigset_t sa_mask;
};

/* ── Alternate signal stack (sigaltstack) ────────────────────────────────── */
#define SS_ONSTACK   1
#define SS_DISABLE   2
#define MINSIGSTKSZ  2048
#define SIGSTKSZ     8192

typedef struct {
    void  *ss_sp;
    int    ss_flags;
    size_t ss_size;
} stack_t;

/* ── Signal manipulation prototypes ───────────────────────────────────────── */
int kill(int pid, int sig);
int raise(int sig);
sighandler_t signal(int signum, sighandler_t handler);
int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact);
int sigprocmask(int how, const sigset_t *set, sigset_t *oldset);
int pthread_sigmask(int how, const sigset_t *set, sigset_t *oldset);
int sigaltstack(const stack_t *ss, stack_t *old_ss);
int sigemptyset(sigset_t *set);
int sigfillset(sigset_t *set);
int sigaddset(sigset_t *set, int signum);
int sigdelset(sigset_t *set, int signum);
int sigismember(const sigset_t *set, int signum);
int sigisemptyset(const sigset_t *set);
int sigorset(sigset_t *dest, const sigset_t *left, const sigset_t *right);
int sigandset(sigset_t *dest, const sigset_t *left, const sigset_t *right);
int sigpending(sigset_t *set);
int sigsuspend(const sigset_t *mask);

/* XSI signal manipulation */
int sighold(int sig);
int sigrelse(int sig);
int sigignore(int sig);
int sigpause(int sig);

/* Diagnostic signal reporting */
void psignal(int sig, const char *s);

/* ── Queued signals and synchronous waiting ─────────────────────────────── */

#ifndef __union_sigval_defined
#define __union_sigval_defined
union sigval {
    int   sival_int;
    void *sival_ptr;
};
#endif

/* <sys/wait.h> forward-declares this as `struct siginfo`, so the tag matters. */
struct siginfo {
    int          si_signo;
    int          si_errno;
    int          si_code;
    int          __pad0;
    int          si_pid;
    unsigned int si_uid;
    union sigval si_value;
    char         __pad[128 - 32];
};
#ifndef __siginfo_t_defined
#define __siginfo_t_defined
typedef struct siginfo siginfo_t;
#endif

void psiginfo(const siginfo_t *pinfo, const char *s);

struct timespec;

/**
 * sigwaitinfo(set, info) → the signal number accepted, or -1.
 *
 * Blocks until one of @set is pending, then removes it from the pending set
 * instead of running its handler.  The signals in @set should be blocked in
 * the caller first, or they may be delivered before this can accept them.
 */
int sigwaitinfo(const sigset_t *set, siginfo_t *info);

/** sigtimedwait(set, info, timeout) — sigwaitinfo() with a deadline (EAGAIN). */
int sigtimedwait(const sigset_t *set, siginfo_t *info,
                 const struct timespec *timeout);

/** sigwait(set, sig) — accept a signal, reporting only its number. */
int sigwait(const sigset_t *set, int *sig);

/** sigqueue(pid, sig, value) — send a signal with an accompanying value. */
int sigqueue(int pid, int sig, const union sigval value);
int siginterrupt(int sig, int flag);
int signalfd(int fd, const sigset_t *mask, int flags);
