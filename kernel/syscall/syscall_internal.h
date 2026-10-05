/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */
/* ============================================================================
 * AzamiOS — Internal System Call Subsystem Definitions
 * File: kernel/syscall/syscall_internal.h
 * ============================================================================ */
#pragma once

#define DEBUG 0
#include "../../include/azami/debug.h"
#include "syscall.h"
#include "syscall_table.h"
#include "../ptrace.h"
#include "../perf/perf.h"
#include "../../drivers/char/console.h"
#include "../../drivers/char/uart.h"
#include "../../drivers/input/input.h"
#include "../../kernel/lib/string.h"
#include "../../kernel/lib/random.h"
#include "../../kernel/sched/sched.h"
#include "../../kernel/sched/elf.h"
#include "../../fs/vfs.h"
#include "../../fs/namespace.h"
#include "../../fs/aio.h"
#include "../../kernel/mm/pmm.h"
#include "../../kernel/mm/kmalloc.h"
#include "../../kernel/mm/vma.h"
#include "../../kernel/signal.h"
#include "../../kernel/ipc/ipc.h"
#include "../../kernel/ipc/sysvipc.h"
#include "../../kernel/ktimer.h"
#include "../../kernel/object/object.h"
#include "../../arch/x86_64/cpu/nospec.h"
#include "../../arch/x86_64/mm/vmm.h"
#include "../../arch/x86_64/boot/limine_req.h"
#include "../../drivers/misc/bga.h"
#include "../../drivers/video/virtio_gpu.h"
#include "../../include/azami/defs.h"
#include "../../kernel/uaccess.h"
#include "../../fs/pipe.h"
#include "../../drivers/acpi/power.h"
#include "../../drivers/misc/rtc.h"
#include "../../drivers/misc/hpet.h"
#include "../../kernel/time/timekeeping.h"
#include "../../include/azami/vdso.h"
#include "../../include/azami/net.h"
#include "../../include/azami/socket.h"
#include "../../include/azami/igmp.h"
#include "../security/acl.h"
#include "../../arch/x86_64/cpu/cpu.h"
#include "../../arch/x86_64/cpu/msr.h"
#include "../security/security.h"
#include "../../arch/x86_64/cpu/smp.h"
#include "../../arch/x86_64/cpu/spinlock.h"
#include "../../arch/x86_64/cpu/hwaccel.h"
#include "../ipc/mqueue.h"
#include "../perf/ktrace.h"
#include <azami/sections.h>

/* Poll event flags */
#ifndef POLLIN
#define POLLIN      0x0001
#define POLLPRI     0x0002
#define POLLOUT     0x0004
#define POLLERR     0x0008
#define POLLHUP     0x0010
#define POLLNVAL    0x0020
#define POLLRDNORM  0x0040
#define POLLRDBAND  0x0080
#define POLLWRNORM  0x0100
#define POLLWRBAND  0x0200
#endif

/* Sleep and timing constants */
#define SLEEP_TICK_NS    (NSEC_PER_SEC / TK_HZ)
#define SLEEP_SPIN_NS    200000ULL           /* 200 µs */
#define SLEEP_HR_SPIN_NS 20000ULL            /* 20 µs, with one-shot timers */
#define SLEEP_MAX_NS     (0x7FFFFFFFFFFFFFFFULL / 2)

/* Standard Linux time structures shared across subsystems */
struct linux_timespec {
    long tv_sec;
    long tv_nsec;
};

struct linux_timeval {
    long tv_sec;
    long tv_usec;
};

struct itimerspec {
    struct linux_timespec it_interval;
    struct linux_timespec it_value;
};

struct k_itimerval {
    struct linux_timeval it_interval;
    struct linux_timeval it_value;
};

/* Scatter-gather I/O vector */
struct iovec {
    void  *iov_base;
    size_t iov_len;
};

/* PIDFD internal context & operations */
typedef struct {
    u32 target_pid;
} pidfd_ctx_t;

extern file_operations_t g_pidfd_fops;

/* Global dispatch table */
extern const syscall_fn_t g_syscall_table[SYSCALL_TABLE_SIZE];

/* File descriptor helpers (implemented in syscall.c) */
file_t *fget(process_t *proc, int fd);
void    fput(file_t *file);
int     fd_limit(process_t *proc);
s64     fd_install_from(process_t *proc, void *file, u8 fd_flags, int minfd);
s64     fd_install(process_t *proc, file_t *file, u8 fd_flags);
s64     fd_install_pair(process_t *proc, void *f0, void *f1, u8 fd_flags, int *out0, int *out1);
file_t *fd_detach(process_t *proc, int fd);

/* User memory & path helpers (implemented in syscall.c) */
s64         copy_str_from_user(char *dst, const char *user_src, size_t max_len);
bool        proc_is_confined(const process_t *proc);
s64         vpath_to_real(const process_t *proc, const char *vpath, char *out, size_t out_len);
const char *real_to_vpath(const process_t *proc, const char *real);
s64         copy_user_vpath_resolve_at(int dirfd, char *vpath, size_t max_len, const char *user_path);
s64         copy_user_path_resolve_at(int dirfd, char *kpath, size_t max_len, const char *user_path);
s64         copy_user_path_resolve(char *kpath, size_t max_len, const char *user_path);

/* Time & sleep helpers */
bool sleep_signal_pending(process_t *proc);
s64  sleep_until_mono(u64 deadline, u64 *left);
u64  get_cached_unix_time(void);

static inline u64 timespec_to_ns(const struct linux_timespec *ts)
{
    return (u64)ts->tv_sec * NSEC_PER_SEC + (u64)ts->tv_nsec;
}

static inline void ns_to_timespec(u64 ns, struct linux_timespec *ts)
{
    ts->tv_sec  = (long)(ns / NSEC_PER_SEC);
    ts->tv_nsec = (long)(ns % NSEC_PER_SEC);
}

static inline u64 timespec_to_ticks(const struct linux_timespec *ts)
{
    u64 ticks = (u64)ts->tv_sec * 100 + (u64)ts->tv_nsec / 10000000ULL;
    if (ticks == 0 && (ts->tv_sec || ts->tv_nsec)) ticks = 1;
    return ticks;
}

static inline void ticks_to_timespec(u64 ticks, struct linux_timespec *ts)
{
    ts->tv_sec  = (long)(ticks / 100);
    ts->tv_nsec = (long)((ticks % 100) * 10000000ULL);
}

static inline u64 timeval_to_ticks(const struct linux_timeval *tv)
{
    u64 ticks = (u64)tv->tv_sec * 100 + (u64)tv->tv_usec / 10000ULL;
    if (ticks == 0 && (tv->tv_sec || tv->tv_usec)) ticks = 1;
    return ticks;
}

static inline void ticks_to_timeval(u64 ticks, struct linux_timeval *tv)
{
    tv->tv_sec  = (long)(ticks / 100);
    tv->tv_usec = (long)((ticks % 100) * 10000ULL);
}

void syscall_report_missing(u64 nr, pt_regs_t *regs);
