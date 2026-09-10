/* ============================================================================
 * AzamiOS libc — XSI and POSIX extensions
 * File: userland/libc/xsi.c
 *
 * The System V IPC family, the POSIX per-process timers, the interval timers
 * and the synchronous signal-waiting calls.  All of these are thin wrappers:
 * the kernel implements the semantics, so the job here is marshalling and the
 * errno convention.
 * ============================================================================ */

#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <sys/msg.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <unistd.h>

/* Every raw syscall returns -errno in the range [-4095, -1]. */
static long xsi_ret(long r)
{
    if (r < 0 && r > -4096) {
        errno = (int)-r;
        return -1;
    }
    return r;
}

/* ── sys/ipc.h ───────────────────────────────────────────────────────────── */

key_t ftok(const char *pathname, int proj_id)
{
    struct stat st;
    if (!pathname || stat(pathname, &st) != 0) return (key_t)-1;

    /* The conventional construction: the low byte of proj_id, the low byte of
     * the device number, and the low 16 bits of the inode. */
    return (key_t)(((unsigned int)(proj_id & 0xFF) << 24) |
                   ((unsigned int)(st.st_dev & 0xFF) << 16) |
                   ((unsigned int)(st.st_ino & 0xFFFF)));
}

/* ── Shared memory ───────────────────────────────────────────────────────── */

int shmget(key_t key, size_t size, int shmflg)
{
    return (int)xsi_ret(syscall3(SYS_shmget, (long)key, (long)size, (long)shmflg));
}

void *shmat(int shmid, const void *shmaddr, int shmflg)
{
    long r = syscall3(SYS_shmat, (long)shmid, (long)shmaddr, (long)shmflg);
    if (r < 0 && r > -4096) {
        errno = (int)-r;
        return (void *)-1;
    }
    return (void *)r;
}

int shmdt(const void *shmaddr)
{
    return (int)xsi_ret(syscall1(SYS_shmdt, (long)shmaddr));
}

int shmctl(int shmid, int cmd, struct shmid_ds *buf)
{
    /* The kernel only implements the 64-bit layout, which is what struct
     * shmid_ds already is here. */
    return (int)xsi_ret(syscall3(SYS_shmctl, (long)shmid, (long)(cmd | IPC_64), (long)buf));
}

/* ── Semaphores ──────────────────────────────────────────────────────────── */

int semget(key_t key, int nsems, int semflg)
{
    return (int)xsi_ret(syscall3(SYS_semget, (long)key, (long)nsems, (long)semflg));
}

int semop(int semid, struct sembuf *sops, size_t nsops)
{
    return (int)xsi_ret(syscall3(SYS_semop, (long)semid, (long)sops, (long)nsops));
}

int semtimedop(int semid, struct sembuf *sops, size_t nsops,
               const struct timespec *timeout)
{
    return (int)xsi_ret(syscall4(SYS_semtimedop, (long)semid, (long)sops,
                                 (long)nsops, (long)timeout));
}

int semctl(int semid, int semnum, int cmd, ...)
{
    /* The fourth argument is a union passed by value, and is absent entirely
     * for the commands that do not use one. */
    long arg = 0;
    switch (cmd & ~IPC_64) {
    case SETVAL:
    case GETALL:
    case SETALL:
    case IPC_STAT:
    case IPC_SET:
    case SEM_STAT:
    case IPC_INFO:
    case SEM_INFO: {
        va_list ap;
        va_start(ap, cmd);
        union semun u = va_arg(ap, union semun);
        va_end(ap);
        arg = (cmd & ~IPC_64) == SETVAL ? (long)u.val : (long)u.__pad;
        break;
    }
    default:
        break;
    }
    return (int)xsi_ret(syscall4(SYS_semctl, (long)semid, (long)semnum,
                                 (long)(cmd | IPC_64), arg));
}

/* ── Message queues ──────────────────────────────────────────────────────── */

int msgget(key_t key, int msgflg)
{
    return (int)xsi_ret(syscall2(SYS_msgget, (long)key, (long)msgflg));
}

int msgsnd(int msqid, const void *msgp, size_t msgsz, int msgflg)
{
    return (int)xsi_ret(syscall4(SYS_msgsnd, (long)msqid, (long)msgp,
                                 (long)msgsz, (long)msgflg));
}

ssize_t msgrcv(int msqid, void *msgp, size_t msgsz, long msgtyp, int msgflg)
{
    return (ssize_t)xsi_ret(syscall5(SYS_msgrcv, (long)msqid, (long)msgp,
                                     (long)msgsz, msgtyp, (long)msgflg));
}

int msgctl(int msqid, int cmd, struct msqid_ds *buf)
{
    return (int)xsi_ret(syscall3(SYS_msgctl, (long)msqid, (long)(cmd | IPC_64), (long)buf));
}

/* ── POSIX per-process timers ────────────────────────────────────────────── */

int timer_create(clockid_t clockid, struct sigevent *sevp, timer_t *timerid)
{
    /* The kernel writes the id as a 32-bit value, which is exactly what
     * timer_t is on this platform. */
    int id = 0;
    long r = syscall3(SYS_timer_create, (long)clockid, (long)sevp, (long)&id);
    if (r < 0 && r > -4096) { errno = (int)-r; return -1; }
    if (timerid) *timerid = (timer_t)id;
    return 0;
}

int timer_settime(timer_t timerid, int flags,
                  const struct itimerspec *new_value, struct itimerspec *old_value)
{
    return (int)xsi_ret(syscall4(SYS_timer_settime, (long)timerid, (long)flags,
                                 (long)new_value, (long)old_value));
}

int timer_gettime(timer_t timerid, struct itimerspec *curr_value)
{
    return (int)xsi_ret(syscall2(SYS_timer_gettime, (long)timerid, (long)curr_value));
}

int timer_getoverrun(timer_t timerid)
{
    return (int)xsi_ret(syscall1(SYS_timer_getoverrun, (long)timerid));
}

int timer_delete(timer_t timerid)
{
    return (int)xsi_ret(syscall1(SYS_timer_delete, (long)timerid));
}

/* ── Interval timers ─────────────────────────────────────────────────────── */

int setitimer(int which, const struct itimerval *new_value, struct itimerval *old_value)
{
    return (int)xsi_ret(syscall3(SYS_setitimer, (long)which,
                                 (long)new_value, (long)old_value));
}

int getitimer(int which, struct itimerval *curr_value)
{
    return (int)xsi_ret(syscall2(SYS_getitimer, (long)which, (long)curr_value));
}

/* ── Synchronous signal waiting ──────────────────────────────────────────── */

int sigtimedwait(const sigset_t *set, siginfo_t *info, const struct timespec *timeout)
{
    return (int)xsi_ret(syscall4(SYS_rt_sigtimedwait, (long)set, (long)info,
                                 (long)timeout, (long)sizeof(sigset_t)));
}

int sigwaitinfo(const sigset_t *set, siginfo_t *info)
{
    return sigtimedwait(set, info, NULL);
}

/* sigwait() itself already lives in signal.c; sigwaitinfo() is the variant
 * that also reports the accompanying siginfo. */

int sigqueue(int pid, int sig, const union sigval value)
{
    siginfo_t info;
    for (unsigned i = 0; i < sizeof(info); i++) ((char *)&info)[i] = 0;
    info.si_signo = sig;
    info.si_code  = -1;          /* SI_QUEUE */
    info.si_pid   = getpid();
    info.si_value = value;

    return (int)xsi_ret(syscall3(SYS_rt_sigqueueinfo, (long)pid, (long)sig, (long)&info));
}
