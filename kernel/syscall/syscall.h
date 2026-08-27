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
#define SYS_lseek         8
#define SYS_mmap          9
#define SYS_mprotect      10
#define SYS_munmap        11
#define SYS_brk           12
#define SYS_rt_sigaction  13
#define SYS_rt_sigprocmask 14
#define SYS_rt_sigreturn  15
#define SYS_ioctl         16
#define SYS_pread64       17
#define SYS_pwrite64      18
#define SYS_readv         19
#define SYS_writev        20
#define SYS_access        21
#define SYS_pipe          22
#define SYS_select        23
#define SYS_sched_yield   24
#define SYS_mremap        25
#define SYS_msync         26
#define SYS_mincore       27
#define SYS_madvise       28
#define SYS_dup           32
#define SYS_dup2          33
#define SYS_pause         34
#define SYS_nanosleep     35
#define SYS_alarm         37
#define SYS_getpid        39
#define SYS_sendfile      40
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
#define SYS_socketpair    53
#define SYS_setsockopt    54
#define SYS_getsockopt    55
#define SYS_clone         56
#define SYS_fork          57
#define SYS_vfork         58
#define SYS_execve        59
#define SYS_exit          60
#define SYS_wait4         61
#define SYS_kill          62
#define SYS_uname         63
#define SYS_fcntl         72
#define SYS_flock         73
#define SYS_fsync         74
#define SYS_fdatasync     75
#define SYS_truncate      76
#define SYS_ftruncate     77
#define SYS_getdents      78
#define SYS_getcwd        79
#define SYS_chdir         80
#define SYS_fchdir        81
#define SYS_rename        82
#define SYS_mkdir         83
#define SYS_rmdir         84
#define SYS_link          86
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
#define SYS_getrlimit     97
#define SYS_getrusage     98
#define SYS_getuid        102
#define SYS_syslog        103
#define SYS_getgid        104
#define SYS_setuid        105
#define SYS_setgid        106
#define SYS_geteuid       107
#define SYS_getegid       108
#define SYS_setpgid       109
#define SYS_getppid       110
#define SYS_getpgrp       111
#define SYS_setsid        112
#define SYS_setreuid      113
#define SYS_setregid      114
#define SYS_getgroups     115
#define SYS_setgroups     116
#define SYS_setresuid     117
#define SYS_getresuid     118
#define SYS_setresgid     119
#define SYS_getresgid     120
#define SYS_getpgid       121
#define SYS_getsid        124
#define SYS_capget        125
#define SYS_capset        126
#define SYS_utime         132
#define SYS_personality   135
#define SYS_statfs        137
#define SYS_fstatfs       138
#define SYS_getpriority   140
#define SYS_setpriority   141
#define SYS_sched_setparam 142
#define SYS_sched_getparam 143
#define SYS_sched_setscheduler 144
#define SYS_sched_getscheduler 145
#define SYS_sched_get_priority_max 146
#define SYS_sched_get_priority_min 147
#define SYS_swapon        167
#define SYS_swapoff       168
#define SYS_sched_rr_get_interval 148
#define SYS_prctl         157
#define SYS_arch_prctl    158
#define SYS_setrlimit     160
#define SYS_chroot        161
#define SYS_sync          162
#define SYS_reboot        169
#define SYS_sethostname   170
#define SYS_setdomainname 171
#define SYS_gettid        186
#define SYS_setxattr      188
#define SYS_lsetxattr     189
#define SYS_fsetxattr     190
#define SYS_getxattr      191
#define SYS_lgetxattr     192
#define SYS_fgetxattr     193
#define SYS_listxattr     194
#define SYS_llistxattr    195
#define SYS_flistxattr    196
#define SYS_removexattr   197
#define SYS_lremovexattr  198
#define SYS_fremovexattr  199
#define SYS_tkill         200
#define SYS_time          201
#define SYS_futex         202
#define SYS_sched_setaffinity 203
#define SYS_sched_getaffinity 204
#define SYS_epoll_create  213
#define SYS_getdents64    217
#define SYS_set_tid_address 218
#define SYS_fadvise64     221
#define SYS_clock_settime 227
#define SYS_clock_gettime 228
#define SYS_clock_getres  229
#define SYS_clock_nanosleep 230
#define SYS_exit_group    231
#define SYS_epoll_wait    232
#define SYS_epoll_ctl     233
#define SYS_tgkill        234
#define SYS_utimes        235
#define SYS_waitid        247
#define SYS_inotify_init  253
#define SYS_inotify_add_watch 254
#define SYS_inotify_rm_watch  255
#define SYS_openat        257
#define SYS_mkdirat       258
#define SYS_fchownat      260
#define SYS_fstatat       262
#define SYS_unlinkat      263
#define SYS_renameat      264
#define SYS_linkat        265
#define SYS_symlinkat     266
#define SYS_readlinkat    267
#define SYS_fchmodat      268
#define SYS_faccessat     269
#define SYS_readahead     187
#define SYS_pselect6      270
#define SYS_ppoll         271
#define SYS_set_robust_list 273
#define SYS_splice        275
#define SYS_tee           276
#define SYS_sync_file_range 277
#define SYS_vmsplice      278
#define SYS_utimensat     280
#define SYS_epoll_pwait   281
#define SYS_signalfd      282
#define SYS_timerfd_create 283
#define SYS_eventfd       284
#define SYS_fallocate     285
#define SYS_timerfd_settime 286
#define SYS_timerfd_gettime 287
#define SYS_signalfd4     289
#define SYS_eventfd2      290
#define SYS_epoll_create1 291
#define SYS_dup3          292
#define SYS_pipe2         293
#define SYS_inotify_init1 294
#define SYS_prlimit64     302
#define SYS_syncfs        306
#define SYS_getcpu        309
#define SYS_kcmp          312
#define SYS_finit_module  313
#define SYS_sched_setattr 314
#define SYS_sched_getattr 315
#define SYS_renameat2     316
#define SYS_seccomp       317
#define SYS_getrandom     318
#define SYS_memfd_create  319
#define SYS_bpf           321
#define SYS_execveat      322
#define SYS_userfaultfd   323
#define SYS_membarrier    324
#define SYS_copy_file_range 326
#define SYS_getfacl       328
#define SYS_setfacl       329
#define SYS_pkey_mprotect 329
#define SYS_pkey_alloc    330
#define SYS_pkey_free     331
#define SYS_statx         332
#define SYS_rseq          334
#define SYS_pidfd_send_signal 424
#define SYS_pidfd_open    434
#define SYS_clone3        435
#define SYS_close_range   436
#define SYS_openat2       437
#define SYS_pidfd_getfd   438
#define SYS_faccessat2    439
#define SYS_epoll_pwait2  441

/* ── Remaining Linux x86_64 syscall numbers (kept in sync with the C
 *    library's <sys/syscall.h>). Those with a real handler are wired up in
 *    syscall_init(); the rest resolve to -ENOSYS through the dispatcher's
 *    default path. Defining every number here lets userland reference and
 *    probe them by name. ─────────────────────────────────────────────────── */
#define SYS_shmget               29
#define SYS_shmat                30
#define SYS_shmctl               31
#define SYS_getitimer            36
#define SYS_setitimer            38
#define SYS_semget               64
#define SYS_semop                65
#define SYS_semctl               66
#define SYS_shmdt                67
#define SYS_msgget               68
#define SYS_msgsnd               69
#define SYS_msgrcv               70
#define SYS_msgctl               71
#define SYS_creat                85
#define SYS_lchown               94
#define SYS_ptrace               101
#define SYS_setfsuid             122
#define SYS_setfsgid             123
#define SYS_rt_sigpending        127
#define SYS_rt_sigtimedwait      128
#define SYS_rt_sigqueueinfo      129
#define SYS_rt_sigsuspend        130
#define SYS_sigaltstack          131
#define SYS_mknod                133
#define SYS_mlock                149
#define SYS_munlock              150
#define SYS_mlockall             151
#define SYS_munlockall           152
#define SYS_adjtimex             159
#define SYS_acct                 163
#define SYS_settimeofday         164
#define SYS_mount                165
#define SYS_umount2              166
#define SYS_iopl                 172
#define SYS_ioperm               173
#define SYS_init_module          175
#define SYS_delete_module        176
#define SYS_io_setup             206
#define SYS_io_destroy           207
#define SYS_io_getevents         208
#define SYS_io_submit            209
#define SYS_io_cancel            210
#define SYS_timer_create         222
#define SYS_timer_settime        223
#define SYS_timer_gettime        224
#define SYS_timer_getoverrun     225
#define SYS_timer_delete         226
#define SYS_mknodat              259
#define SYS_futimesat            261
#define SYS_unshare              272
#define SYS_get_robust_list      274
#define SYS_move_pages           279
#define SYS_preadv               295
#define SYS_pwritev              296
#define SYS_rt_tgsigqueueinfo    297
#define SYS_perf_event_open      298
#define SYS_recvmmsg             299
#define SYS_fanotify_init        300
#define SYS_fanotify_mark        301
#define SYS_name_to_handle_at    303
#define SYS_open_by_handle_at    304
#define SYS_clock_adjtime        305
#define SYS_sendmmsg             307
#define SYS_setns                308
#define SYS_process_vm_readv     310
#define SYS_process_vm_writev    311
#define SYS_kexec_file_load      320
#define SYS_mlock2               325
#define SYS_preadv2              327
#define SYS_pwritev2             328

/* Linux statx timestamp and structure */
struct statx_timestamp {
    s64 tv_sec;
    u32 tv_nsec;
    s32 __reserved;
};

struct statx {
    u32 stx_mask;
    u32 stx_blksize;
    u64 stx_attributes;
    u32 stx_nlink;
    u32 stx_uid;
    u32 stx_gid;
    u16 stx_mode;
    u16 __spare0[1];
    u64 stx_ino;
    u64 stx_size;
    u64 stx_blocks;
    u64 stx_attributes_mask;
    struct statx_timestamp stx_atime;
    struct statx_timestamp stx_btime;
    struct statx_timestamp stx_ctime;
    struct statx_timestamp stx_mtime;
    u32 stx_rdev_major;
    u32 stx_rdev_minor;
    u32 stx_dev_major;
    u32 stx_dev_minor;
    u64 stx_mnt_id;
    u64 __spare2;
    u64 __spare3[12];
};

#define STATX_TYPE          0x00000001U
#define STATX_MODE          0x00000002U
#define STATX_NLINK         0x00000004U
#define STATX_UID           0x00000008U
#define STATX_GID           0x00000010U
#define STATX_ATIME         0x00000020U
#define STATX_MTIME         0x00000040U
#define STATX_CTIME         0x00000080U
#define STATX_INO           0x00000100U
#define STATX_SIZE          0x00000200U
#define STATX_BLOCKS        0x00000400U
#define STATX_BASIC_STATS   0x000007ffU
#define STATX_BTIME         0x00000800U
#define STATX_ALL           0x00000fffU

/* POSIX *at flags */
#define AT_FDCWD            (-100)
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_REMOVEDIR        0x200
#define AT_EMPTY_PATH       0x1000
#define AT_STATX_SYNC_AS_STAT 0x0000
#define AT_STATX_FORCE_SYNC   0x2000
#define AT_STATX_DONT_SYNC    0x4000

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
#define SYS_AZ_THREAD_EXIT     530  /* Terminate calling thread cleanly */
#define SYS_AZ_FB_FLIP         531  /* Hardware zero-copy display buffer flip */

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
