/* ============================================================================
 * AzamiOS Userspace — Linux-specific system call wrappers
 * File: userland/libc/linuxext.c
 *
 * Wrappers for the Linux calls that are not part of POSIX and that glibc
 * therefore leaves to syscall(): the futex2 trio, I/O priorities, NUMA memory
 * policy, and the two mount/terminal administration calls. Having them here
 * means a program can call them by name and get argument checking, rather than
 * open-coding a syscall number and casting six longs.
 * ============================================================================ */

#include "include/sys/syscall.h"
#include "include/sys/ioprio.h"
#include "include/linux/futex.h"
#include "include/numaif.h"
#include "include/unistd.h"
#include "include/errno.h"

static inline long __lx_ret(long r)
{
    if (r < 0) { errno = (int)-r; return -1; }
    return r;
}

/* ── futex2 ──────────────────────────────────────────────────────────────── */
/* futex(2) itself already has a wrapper in syscall.c; only the futex2 calls
 * are new. */

int futex_wake(void *uaddr, unsigned long mask, int nr, unsigned int flags)
{
    return (int)__lx_ret(syscall4(SYS_futex_wake, (long)uaddr, (long)mask,
                                  nr, (long)flags));
}

int futex_wait(void *uaddr, unsigned long val, unsigned long mask,
               unsigned int flags, struct timespec *timeout, clockid_t clockid)
{
    return (int)__lx_ret(syscall6(SYS_futex_wait, (long)uaddr, (long)val,
                                  (long)mask, (long)flags, (long)timeout,
                                  (long)clockid));
}

int futex_requeue(struct futex_waitv *waiters, unsigned int flags,
                  int nr_wake, int nr_requeue)
{
    return (int)__lx_ret(syscall4(SYS_futex_requeue, (long)waiters,
                                  (long)flags, nr_wake, nr_requeue));
}

int futex_waitv(struct futex_waitv *waiters, unsigned int nr_futexes,
                unsigned int flags, struct timespec *timeout, clockid_t clockid)
{
    return (int)__lx_ret(syscall5(SYS_futex_waitv, (long)waiters,
                                  (long)nr_futexes, (long)flags,
                                  (long)timeout, (long)clockid));
}

/* ── I/O priority ────────────────────────────────────────────────────────── */

int ioprio_set(int which, int who, int ioprio)
{
    return (int)__lx_ret(syscall3(SYS_ioprio_set, which, who, ioprio));
}

int ioprio_get(int which, int who)
{
    return (int)__lx_ret(syscall2(SYS_ioprio_get, which, who));
}

/* ── NUMA memory policy ──────────────────────────────────────────────────── */

int set_mempolicy(int mode, const unsigned long *nodemask,
                  unsigned long maxnode)
{
    return (int)__lx_ret(syscall3(SYS_set_mempolicy, mode, (long)nodemask,
                                  (long)maxnode));
}

int get_mempolicy(int *mode, unsigned long *nodemask, unsigned long maxnode,
                  void *addr, unsigned long flags)
{
    return (int)__lx_ret(syscall5(SYS_get_mempolicy, (long)mode,
                                  (long)nodemask, (long)maxnode,
                                  (long)addr, (long)flags));
}

int mbind(void *addr, unsigned long len, int mode,
          const unsigned long *nodemask, unsigned long maxnode,
          unsigned int flags)
{
    return (int)__lx_ret(syscall6(SYS_mbind, (long)addr, (long)len, mode,
                                  (long)nodemask, (long)maxnode, (long)flags));
}

long migrate_pages(int pid, unsigned long maxnode,
                   const unsigned long *old_nodes,
                   const unsigned long *new_nodes)
{
    return __lx_ret(syscall4(SYS_migrate_pages, pid, (long)maxnode,
                             (long)old_nodes, (long)new_nodes));
}

int set_mempolicy_home_node(void *start, unsigned long len,
                            unsigned long home_node, unsigned long flags)
{
    return (int)__lx_ret(syscall4(SYS_set_mempolicy_home_node, (long)start,
                                  (long)len, (long)home_node, (long)flags));
}

long move_pages(int pid, unsigned long count, void **pages,
                const int *nodes, int *status, int flags)
{
    return __lx_ret(syscall6(SYS_move_pages, pid, (long)count, (long)pages,
                             (long)nodes, (long)status, (long)flags));
}

/* ── System administration ───────────────────────────────────────────────── */

int pivot_root(const char *new_root, const char *put_old)
{
    return (int)__lx_ret(syscall2(SYS_pivot_root, (long)new_root,
                                  (long)put_old));
}

int vhangup(void)
{
    return (int)__lx_ret(syscall0(SYS_vhangup));
}

int process_mrelease(int pidfd, unsigned int flags)
{
    return (int)__lx_ret(syscall2(SYS_process_mrelease, pidfd, (long)flags));
}
