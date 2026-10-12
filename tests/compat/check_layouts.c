/* SPDX-License-Identifier: MIT */
/* ============================================================================
 * AzamiOS — Compile-Time and Runtime UAPI Struct Layout Verification Suite
 * File: tests/compat/check_layouts.c
 *
 * Implements Workstream B3.1: asserts that struct sizes and member offsets
 * match the Linux 6.6 x86-64 ABI bit-for-bit.
 * ============================================================================ */
#define _GNU_SOURCE
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/utsname.h>
#include <sys/epoll.h>
#include <sys/uio.h>
#include <sys/socket.h>
#include <sys/sysinfo.h>
#include <sys/vfs.h>
#include <sys/user.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <netinet/in.h>
#include <sys/un.h>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <ucontext.h>
#if defined(__has_include)
# if __has_include(<linux/openat2.h>)
#  include <linux/openat2.h>
# endif
# if __has_include(<linux/sched.h>)
#  include <linux/sched.h>
# endif
# if __has_include(<linux/sched/types.h>)
#  include <linux/sched/types.h>
# endif
#endif

#include "layouts.h"

/* Compile-time assertions for standard structures */
ASSERT_LAYOUT_SIZE(struct stat, LINUX_SIZEOF_STAT);
ASSERT_LAYOUT_OFFSET(struct stat, st_dev, LINUX_OFFSETOF_STAT_ST_DEV);
ASSERT_LAYOUT_OFFSET(struct stat, st_ino, LINUX_OFFSETOF_STAT_ST_INO);
ASSERT_LAYOUT_OFFSET(struct stat, st_nlink, LINUX_OFFSETOF_STAT_ST_NLINK);
ASSERT_LAYOUT_OFFSET(struct stat, st_mode, LINUX_OFFSETOF_STAT_ST_MODE);
ASSERT_LAYOUT_OFFSET(struct stat, st_uid, LINUX_OFFSETOF_STAT_ST_UID);
ASSERT_LAYOUT_OFFSET(struct stat, st_gid, LINUX_OFFSETOF_STAT_ST_GID);
ASSERT_LAYOUT_OFFSET(struct stat, st_rdev, LINUX_OFFSETOF_STAT_ST_RDEV);
ASSERT_LAYOUT_OFFSET(struct stat, st_size, LINUX_OFFSETOF_STAT_ST_SIZE);
ASSERT_LAYOUT_OFFSET(struct stat, st_blksize, LINUX_OFFSETOF_STAT_ST_BLKSIZE);
ASSERT_LAYOUT_OFFSET(struct stat, st_blocks, LINUX_OFFSETOF_STAT_ST_BLOCKS);
ASSERT_LAYOUT_OFFSET(struct stat, st_atim, LINUX_OFFSETOF_STAT_ST_ATIM);
ASSERT_LAYOUT_OFFSET(struct stat, st_mtim, LINUX_OFFSETOF_STAT_ST_MTIM);
ASSERT_LAYOUT_OFFSET(struct stat, st_ctim, LINUX_OFFSETOF_STAT_ST_CTIM);

ASSERT_LAYOUT_SIZE(struct timespec, LINUX_SIZEOF_TIMESPEC);
ASSERT_LAYOUT_OFFSET(struct timespec, tv_sec, LINUX_OFFSETOF_TIMESPEC_TV_SEC);
ASSERT_LAYOUT_OFFSET(struct timespec, tv_nsec, LINUX_OFFSETOF_TIMESPEC_TV_NSEC);

ASSERT_LAYOUT_SIZE(struct itimerspec, LINUX_SIZEOF_ITIMERSPEC);
ASSERT_LAYOUT_OFFSET(struct itimerspec, it_interval, LINUX_OFFSETOF_ITIMERSPEC_IT_INTERVAL);
ASSERT_LAYOUT_OFFSET(struct itimerspec, it_value, LINUX_OFFSETOF_ITIMERSPEC_IT_VALUE);

ASSERT_LAYOUT_SIZE(struct timeval, LINUX_SIZEOF_TIMEVAL);
ASSERT_LAYOUT_OFFSET(struct timeval, tv_sec, LINUX_OFFSETOF_TIMEVAL_TV_SEC);
ASSERT_LAYOUT_OFFSET(struct timeval, tv_usec, LINUX_OFFSETOF_TIMEVAL_TV_USEC);

ASSERT_LAYOUT_SIZE(struct itimerval, LINUX_SIZEOF_ITIMERVAL);
ASSERT_LAYOUT_OFFSET(struct itimerval, it_interval, LINUX_OFFSETOF_ITIMERVAL_IT_INTERVAL);
ASSERT_LAYOUT_OFFSET(struct itimerval, it_value, LINUX_OFFSETOF_ITIMERVAL_IT_VALUE);

ASSERT_LAYOUT_SIZE(struct rlimit, LINUX_SIZEOF_RLIMIT);
ASSERT_LAYOUT_OFFSET(struct rlimit, rlim_cur, LINUX_OFFSETOF_RLIMIT_RLIM_CUR);
ASSERT_LAYOUT_OFFSET(struct rlimit, rlim_max, LINUX_OFFSETOF_RLIMIT_RLIM_MAX);

#if defined(__GLIBC__) || defined(__AZAMI__)
ASSERT_LAYOUT_SIZE(struct rlimit64, LINUX_SIZEOF_RLIMIT64);
ASSERT_LAYOUT_OFFSET(struct rlimit64, rlim_cur, LINUX_OFFSETOF_RLIMIT64_RLIM_CUR);
ASSERT_LAYOUT_OFFSET(struct rlimit64, rlim_max, LINUX_OFFSETOF_RLIMIT64_RLIM_MAX);

ASSERT_LAYOUT_SIZE(struct rusage, LINUX_SIZEOF_RUSAGE);
ASSERT_LAYOUT_OFFSET(struct rusage, ru_utime, LINUX_OFFSETOF_RUSAGE_RU_UTIME);
ASSERT_LAYOUT_OFFSET(struct rusage, ru_stime, LINUX_OFFSETOF_RUSAGE_RU_STIME);
ASSERT_LAYOUT_OFFSET(struct rusage, ru_maxrss, LINUX_OFFSETOF_RUSAGE_RU_MAXRSS);

ASSERT_LAYOUT_SIZE(struct sysinfo, LINUX_SIZEOF_SYSINFO);
ASSERT_LAYOUT_OFFSET(struct sysinfo, uptime, LINUX_OFFSETOF_SYSINFO_UPTIME);
ASSERT_LAYOUT_OFFSET(struct sysinfo, totalram, LINUX_OFFSETOF_SYSINFO_TOTALRAM);
#endif

ASSERT_LAYOUT_SIZE(struct utsname, LINUX_SIZEOF_UTSNAME);
ASSERT_LAYOUT_OFFSET(struct utsname, sysname, LINUX_OFFSETOF_UTSNAME_SYSNAME);
ASSERT_LAYOUT_OFFSET(struct utsname, nodename, LINUX_OFFSETOF_UTSNAME_NODENAME);
ASSERT_LAYOUT_OFFSET(struct utsname, release, LINUX_OFFSETOF_UTSNAME_RELEASE);

ASSERT_LAYOUT_SIZE(struct epoll_event, LINUX_SIZEOF_EPOLL_EVENT);
ASSERT_LAYOUT_OFFSET(struct epoll_event, events, LINUX_OFFSETOF_EPOLL_EVENT_EVENTS);
ASSERT_LAYOUT_OFFSET(struct epoll_event, data, LINUX_OFFSETOF_EPOLL_EVENT_DATA);

ASSERT_LAYOUT_SIZE(struct iovec, LINUX_SIZEOF_IOVEC);
ASSERT_LAYOUT_OFFSET(struct iovec, iov_base, LINUX_OFFSETOF_IOVEC_IOV_BASE);
ASSERT_LAYOUT_OFFSET(struct iovec, iov_len, LINUX_OFFSETOF_IOVEC_IOV_LEN);

ASSERT_LAYOUT_SIZE(struct msghdr, LINUX_SIZEOF_MSGHDR);
ASSERT_LAYOUT_OFFSET(struct msghdr, msg_name, LINUX_OFFSETOF_MSGHDR_MSG_NAME);
ASSERT_LAYOUT_OFFSET(struct msghdr, msg_iov, LINUX_OFFSETOF_MSGHDR_MSG_IOV);
ASSERT_LAYOUT_OFFSET(struct msghdr, msg_control, LINUX_OFFSETOF_MSGHDR_MSG_CONTROL);

ASSERT_LAYOUT_SIZE(struct cmsghdr, LINUX_SIZEOF_CMSGHDR);
ASSERT_LAYOUT_OFFSET(struct cmsghdr, cmsg_len, LINUX_OFFSETOF_CMSGHDR_CMSG_LEN);
ASSERT_LAYOUT_OFFSET(struct cmsghdr, cmsg_level, LINUX_OFFSETOF_CMSGHDR_CMSG_LEVEL);
ASSERT_LAYOUT_OFFSET(struct cmsghdr, cmsg_type, LINUX_OFFSETOF_CMSGHDR_CMSG_TYPE);

ASSERT_LAYOUT_SIZE(struct sockaddr, LINUX_SIZEOF_SOCKADDR);
ASSERT_LAYOUT_OFFSET(struct sockaddr, sa_family, LINUX_OFFSETOF_SOCKADDR_SA_FAMILY);

ASSERT_LAYOUT_SIZE(struct sockaddr_in, LINUX_SIZEOF_SOCKADDR_IN);
ASSERT_LAYOUT_OFFSET(struct sockaddr_in, sin_family, LINUX_OFFSETOF_SOCKADDR_IN_SIN_FAMILY);
ASSERT_LAYOUT_OFFSET(struct sockaddr_in, sin_port, LINUX_OFFSETOF_SOCKADDR_IN_SIN_PORT);
ASSERT_LAYOUT_OFFSET(struct sockaddr_in, sin_addr, LINUX_OFFSETOF_SOCKADDR_IN_SIN_ADDR);

ASSERT_LAYOUT_SIZE(struct sockaddr_in6, LINUX_SIZEOF_SOCKADDR_IN6);
ASSERT_LAYOUT_OFFSET(struct sockaddr_in6, sin6_family, LINUX_OFFSETOF_SOCKADDR_IN6_SIN6_FAMILY);
ASSERT_LAYOUT_OFFSET(struct sockaddr_in6, sin6_port, LINUX_OFFSETOF_SOCKADDR_IN6_SIN6_PORT);
ASSERT_LAYOUT_OFFSET(struct sockaddr_in6, sin6_addr, LINUX_OFFSETOF_SOCKADDR_IN6_SIN6_ADDR);

ASSERT_LAYOUT_SIZE(struct sockaddr_un, LINUX_SIZEOF_SOCKADDR_UN);
ASSERT_LAYOUT_OFFSET(struct sockaddr_un, sun_family, LINUX_OFFSETOF_SOCKADDR_UN_SUN_FAMILY);
ASSERT_LAYOUT_OFFSET(struct sockaddr_un, sun_path, LINUX_OFFSETOF_SOCKADDR_UN_SUN_PATH);

ASSERT_LAYOUT_SIZE(struct statfs, LINUX_SIZEOF_STATFS);
ASSERT_LAYOUT_OFFSET(struct statfs, f_type, LINUX_OFFSETOF_STATFS_F_TYPE);
ASSERT_LAYOUT_OFFSET(struct statfs, f_bsize, LINUX_OFFSETOF_STATFS_F_BSIZE);
ASSERT_LAYOUT_OFFSET(struct statfs, f_blocks, LINUX_OFFSETOF_STATFS_F_BLOCKS);
ASSERT_LAYOUT_OFFSET(struct statfs, f_fsid, LINUX_OFFSETOF_STATFS_F_FSID);
ASSERT_LAYOUT_OFFSET(struct statfs, f_namelen, LINUX_OFFSETOF_STATFS_F_NAMELEN);
ASSERT_LAYOUT_OFFSET(struct statfs, f_flags, LINUX_OFFSETOF_STATFS_F_FLAGS);

ASSERT_LAYOUT_SIZE(struct user_regs_struct, LINUX_SIZEOF_USER_REGS_STRUCT);
ASSERT_LAYOUT_OFFSET(struct user_regs_struct, r15, LINUX_OFFSETOF_USER_REGS_STRUCT_R15);
ASSERT_LAYOUT_OFFSET(struct user_regs_struct, rax, LINUX_OFFSETOF_USER_REGS_STRUCT_RAX);
ASSERT_LAYOUT_OFFSET(struct user_regs_struct, orig_rax, LINUX_OFFSETOF_USER_REGS_STRUCT_ORIG_RAX);
ASSERT_LAYOUT_OFFSET(struct user_regs_struct, rip, LINUX_OFFSETOF_USER_REGS_STRUCT_RIP);
ASSERT_LAYOUT_OFFSET(struct user_regs_struct, rsp, LINUX_OFFSETOF_USER_REGS_STRUCT_RSP);

#ifdef LINUX_SIZEOF_OPEN_HOW
ASSERT_LAYOUT_SIZE(struct open_how, LINUX_SIZEOF_OPEN_HOW);
ASSERT_LAYOUT_OFFSET(struct open_how, flags, LINUX_OFFSETOF_OPEN_HOW_FLAGS);
ASSERT_LAYOUT_OFFSET(struct open_how, mode, LINUX_OFFSETOF_OPEN_HOW_MODE);
ASSERT_LAYOUT_OFFSET(struct open_how, resolve, LINUX_OFFSETOF_OPEN_HOW_RESOLVE);
#endif

#ifdef LINUX_SIZEOF_CLONE_ARGS
ASSERT_LAYOUT_SIZE(struct clone_args, LINUX_SIZEOF_CLONE_ARGS);
ASSERT_LAYOUT_OFFSET(struct clone_args, flags, LINUX_OFFSETOF_CLONE_ARGS_FLAGS);
ASSERT_LAYOUT_OFFSET(struct clone_args, pidfd, LINUX_OFFSETOF_CLONE_ARGS_PIDFD);
ASSERT_LAYOUT_OFFSET(struct clone_args, child_tid, LINUX_OFFSETOF_CLONE_ARGS_CHILD_TID);
ASSERT_LAYOUT_OFFSET(struct clone_args, parent_tid, LINUX_OFFSETOF_CLONE_ARGS_PARENT_TID);
ASSERT_LAYOUT_OFFSET(struct clone_args, exit_signal, LINUX_OFFSETOF_CLONE_ARGS_EXIT_SIGNAL);
ASSERT_LAYOUT_OFFSET(struct clone_args, stack, LINUX_OFFSETOF_CLONE_ARGS_STACK);
ASSERT_LAYOUT_OFFSET(struct clone_args, stack_size, LINUX_OFFSETOF_CLONE_ARGS_STACK_SIZE);
ASSERT_LAYOUT_OFFSET(struct clone_args, tls, LINUX_OFFSETOF_CLONE_ARGS_TLS);
#endif

#ifdef LINUX_SIZEOF_SCHED_ATTR
ASSERT_LAYOUT_SIZE(struct sched_attr, LINUX_SIZEOF_SCHED_ATTR);
ASSERT_LAYOUT_OFFSET(struct sched_attr, size, LINUX_OFFSETOF_SCHED_ATTR_SIZE);
ASSERT_LAYOUT_OFFSET(struct sched_attr, sched_policy, LINUX_OFFSETOF_SCHED_ATTR_SCHED_POLICY);
ASSERT_LAYOUT_OFFSET(struct sched_attr, sched_flags, LINUX_OFFSETOF_SCHED_ATTR_SCHED_FLAGS);
ASSERT_LAYOUT_OFFSET(struct sched_attr, sched_nice, LINUX_OFFSETOF_SCHED_ATTR_SCHED_NICE);
ASSERT_LAYOUT_OFFSET(struct sched_attr, sched_priority, LINUX_OFFSETOF_SCHED_ATTR_SCHED_PRIORITY);
ASSERT_LAYOUT_OFFSET(struct sched_attr, sched_runtime, LINUX_OFFSETOF_SCHED_ATTR_SCHED_RUNTIME);
ASSERT_LAYOUT_OFFSET(struct sched_attr, sched_deadline, LINUX_OFFSETOF_SCHED_ATTR_SCHED_DEADLINE);
ASSERT_LAYOUT_OFFSET(struct sched_attr, sched_period, LINUX_OFFSETOF_SCHED_ATTR_SCHED_PERIOD);
#endif

int main(void)
{
    printf("== AzamiOS UAPI Layout Verification (Workstream B3.1) ==\n");
    printf("  ✓ struct stat: %zu B (expected %d)\n", sizeof(struct stat), LINUX_SIZEOF_STAT);
    printf("  ✓ struct epoll_event: %zu B (expected %d, packed)\n", sizeof(struct epoll_event), LINUX_SIZEOF_EPOLL_EVENT);
    printf("  ✓ struct statfs: %zu B (expected %d)\n", sizeof(struct statfs), LINUX_SIZEOF_STATFS);
    printf("  ✓ struct rusage: %zu B (expected %d)\n", sizeof(struct rusage), LINUX_SIZEOF_RUSAGE);
    printf("  ✓ struct user_regs_struct: %zu B (expected %d)\n", sizeof(struct user_regs_struct), LINUX_SIZEOF_USER_REGS_STRUCT);
    printf("  ✓ struct utsname: %zu B (expected %d)\n", sizeof(struct utsname), LINUX_SIZEOF_UTSNAME);
    printf("  ✓ struct msghdr: %zu B (expected %d)\n", sizeof(struct msghdr), LINUX_SIZEOF_MSGHDR);
    printf("  ✓ struct sockaddr_in: %zu B (expected %d)\n", sizeof(struct sockaddr_in), LINUX_SIZEOF_SOCKADDR_IN);
    printf("  ✓ struct sockaddr_in6: %zu B (expected %d)\n", sizeof(struct sockaddr_in6), LINUX_SIZEOF_SOCKADDR_IN6);
    printf("  ✓ struct sockaddr_un: %zu B (expected %d)\n", sizeof(struct sockaddr_un), LINUX_SIZEOF_SOCKADDR_UN);
#ifdef LINUX_SIZEOF_OPEN_HOW
    printf("  ✓ struct open_how: %zu B (expected %d)\n", sizeof(struct open_how), LINUX_SIZEOF_OPEN_HOW);
#endif
#ifdef LINUX_SIZEOF_CLONE_ARGS
    printf("  ✓ struct clone_args: %zu B (expected %d)\n", sizeof(struct clone_args), LINUX_SIZEOF_CLONE_ARGS);
#endif
#ifdef LINUX_SIZEOF_SCHED_ATTR
    printf("  ✓ struct sched_attr: %zu B (expected %d)\n", sizeof(struct sched_attr), LINUX_SIZEOF_SCHED_ATTR);
#endif
    printf("== All static assertions verified: PASS ==\n");
    return 0;
}
