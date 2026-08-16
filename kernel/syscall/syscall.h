/* ============================================================================
 * AzamiOS — System Call Dispatcher
 * File: kernel/syscall/syscall.h
 * ============================================================================ */
#pragma once

#include "../../include/azami/types.h"
#include "../../arch/x86_64/cpu/idt.h"   /* pt_regs_t */

/* ── Linux-compatible syscall numbers (x86_64 ABI) ────────────────────────── */
#define SYS_read          0
#define SYS_write         1
#define SYS_open          2
#define SYS_close         3
#define SYS_stat          4
#define SYS_fstat         5
#define SYS_lstat         6
#define SYS_poll          7
#define SYS_waitpid       7
#define SYS_lseek         8
#define SYS_mmap          9
#define SYS_munmap        11
#define SYS_brk           12
#define SYS_rt_sigaction  13
#define SYS_rt_sigprocmask 14
#define SYS_rt_sigreturn  15
#define SYS_ioctl         16
#define SYS_readv         19
#define SYS_writev        20
#define SYS_access        21
#define SYS_pipe          22
#define SYS_select        23
#define SYS_dup           32
#define SYS_dup2          33
#define SYS_pause         34
#define SYS_nanosleep     35
#define SYS_alarm         37
#define SYS_getpid        39
#define SYS_socket        41
#define SYS_connect       42
#define SYS_accept        43
#define SYS_sendto        44
#define SYS_recvfrom      45
#define SYS_sendmsg       46
#define SYS_recvmsg       47
#define SYS_shutdown      48
#define SYS_bind          49
#define SYS_listen        50
#define SYS_getsockname   51
#define SYS_getpeername   52
#define SYS_setsockopt    54
#define SYS_getsockopt    55
#define SYS_fork          57
#define SYS_execve        59
#define SYS_exit          60
#define SYS_wait4         61
#define SYS_kill          62
#define SYS_uname         63
#define SYS_fcntl         72
#define SYS_truncate      76
#define SYS_ftruncate     77
#define SYS_getdents      78
#define SYS_getcwd        79
#define SYS_chdir         80
#define SYS_rename        82
#define SYS_mkdir         83
#define SYS_rmdir         84
#define SYS_unlink        87
#define SYS_symlink       88
#define SYS_readlink      89
#define SYS_chmod         90
#define SYS_fchmod        91
#define SYS_chown         92
#define SYS_fchown        93
#define SYS_umask         95
#define SYS_gettimeofday  96
#define SYS_sysinfo       99
#define SYS_times         100
#define SYS_getuid        102
#define SYS_getgid        104
#define SYS_setuid        105
#define SYS_setgid        106
#define SYS_geteuid       107
#define SYS_getegid       108
#define SYS_setpgid       109
#define SYS_getppid       110
#define SYS_getpgrp       111
#define SYS_setsid        112
#define SYS_utime         132
#define SYS_statfs        137
#define SYS_fstatfs       138
#define SYS_reboot        169
#define SYS_time          201
#define SYS_getdents64    217
#define SYS_clock_gettime 228
#define SYS_exit_group    231
#define SYS_utimes        235
#define SYS_pselect6      270
#define SYS_ppoll         271
#define SYS_utimensat     280
#define SYS_dup3          292
#define SYS_pipe2         293

/* Azami-specific extended calls (base 512 to avoid Linux conflicts) */
#define SYS_AZ_CHANNEL_CREATE  512
#define SYS_AZ_CHANNEL_SEND    513
#define SYS_AZ_CHANNEL_RECV    514
#define SYS_AZ_SHMEM_CREATE    515
#define SYS_AZ_SHMEM_MAP       516
#define SYS_AZ_OBJECT_CREATE   517
#define SYS_AZ_OBJECT_OPEN     518
#define SYS_AZ_OBJECT_CLOSE    519
#define SYS_AZ_FB_INFO         520
#define SYS_AZ_FB_MAP          521
#define SYS_AZ_SPAWN           522
#define SYS_AZ_YIELD           523
#define SYS_AZ_THREAD_CREATE   524
#define SYS_AZ_SYSSTAT         525
#define SYS_AZ_SHMEM_DESTROY   526
#define SYS_AZ_SHMEM_UNMAP     527
#define SYS_AZ_CHANNEL_DESTROY 528
#define SYS_AZ_SET_TIMER       529  /* Set a periodic/one-shot IPC timer */

/* ── System Telemetry (SYS_AZ_SYSSTAT) ──────────────────────────────────── */
typedef struct {
    u64 idle_ticks[16];
    u64 active_ticks[16];
} az_sysstat_t;

/* ── Framebuffer info structure (returned by SYS_AZ_FB_INFO) ───────────────── */
typedef struct {
    u32 width;
    u32 height;
    u32 pitch;
    u8  bpp;
    u8  _pad[3];
    u64 phys_addr;    /* Physical address of the framebuffer (for reference) */
} az_fb_info_t;

/**
 * syscall_dispatch() — Main C-side syscall handler.
 * Called from syscall_entry.asm with the full register frame.
 * Return value is placed in regs->rax by this function.
 */
void syscall_dispatch(pt_regs_t *regs);

/**
 * sys_exit_impl() — Exit current process and free resources.
 */
s64 sys_exit_impl(pt_regs_t *r);

/**
 * syscall_init() — Register all syscall handlers.
 * Called from kernel_main() after the scheduler is up.
 */
void syscall_init(void);
