/* ============================================================================
 * AzamiOS — Canonical syscall-number / uapi-struct header
 * File: include/azami/uapi/syscall_nr.h
 *
 * Single source of truth for every SYS_* number and Linux/Azami uapi struct
 * shared between the kernel (kernel/syscall/syscall.h) and the native libc
 * (userland/libc/include/sys/syscall.h, via a build-time copy into
 * userland/libc/include/azami/uapi/ — see userland/Makefile's `uapi-sync`
 * target and scripts/check_uapi_sync.sh).
 *
 * This header is freestanding-safe: it only uses <stdint.h> fixed-width
 * types, never azami/types.h or any other kernel-only header, so it can be
 * included unmodified from -ffreestanding userland code.
 *
 * Do not hand-edit a second copy of this file's contents anywhere else —
 * every consumer must #include this file (directly, or via the staged copy)
 * rather than redefining any of these names.
 * ============================================================================ */
#pragma once

#include <stdint.h>

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
#define SYS_semtimedop    220
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
#define SYS_pkey_mprotect 329
#define SYS_pkey_alloc    330
#define SYS_pkey_free     331
#define SYS_statx         332
#define SYS_rseq          334
#define SYS_process_madvise 440
#define SYS_futex_waitv   449
#define SYS_cachestat     451
#define SYS_mseal         462
#define SYS_pidfd_send_signal 424
#define SYS_pidfd_open    434
#define SYS_clone3        435
#define SYS_close_range   436
#define SYS_openat2       437
#define SYS_pidfd_getfd   438
#define SYS_faccessat2    439
#define SYS_epoll_pwait2  441

/* ── Remaining Linux x86_64 syscall numbers. Those with a real handler are
 *    wired up in syscall_init(); the rest resolve to -ENOSYS through the
 *    dispatcher's default path. Defining every number here lets userland
 *    reference and probe them by name. ─────────────────────────────────── */
#define SYS_shmget               29
#define SYS_shmat                30
#define SYS_shmctl                31
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
#define SYS_mlockall              151
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
#define SYS_pwritev2              328

/* ── POSIX.1-2008 Message Passing (mq_*) ─────────────────────────────────── */
#define SYS_mq_open              240
#define SYS_mq_unlink            241
#define SYS_mq_timedsend         242
#define SYS_mq_timedreceive      243
#define SYS_mq_notify            244
#define SYS_mq_getsetattr        245

/* ── Further Linux calls with real handlers ──────────────────────────────── */
#define SYS_restart_syscall      219
#define SYS_mbind                237
#define SYS_set_mempolicy        238
#define SYS_get_mempolicy        239
#define SYS_ioprio_set           251
#define SYS_ioprio_get           252
#define SYS_migrate_pages        256
#define SYS_accept4              288
#define SYS_pivot_root           155
#define SYS_vhangup              153
#define SYS_process_mrelease     448
#define SYS_set_mempolicy_home_node 450
#define SYS_fchmodat2            452
#define SYS_futex_wake           454
#define SYS_futex_wait           455
#define SYS_futex_requeue        456

/* ioprio_set/get: `which` selects the target, `who` identifies it, and the
 * class/data pair packs into one int as Linux defines it. */
#define IOPRIO_WHO_PROCESS   1
#define IOPRIO_WHO_PGRP      2
#define IOPRIO_WHO_USER      3
#define IOPRIO_CLASS_NONE    0
#define IOPRIO_CLASS_RT      1
#define IOPRIO_CLASS_BE      2
#define IOPRIO_CLASS_IDLE    3
#define IOPRIO_CLASS_SHIFT   13
#define IOPRIO_PRIO_MASK     ((1u << IOPRIO_CLASS_SHIFT) - 1)
#define IOPRIO_NR_LEVELS     8
#define IOPRIO_PRIO_CLASS(v) (((v) >> IOPRIO_CLASS_SHIFT) & 0x7)
#define IOPRIO_PRIO_DATA(v)  ((v) & IOPRIO_PRIO_MASK)
#define IOPRIO_PRIO_VALUE(c, d) ((((c) & 0x7) << IOPRIO_CLASS_SHIFT) | \
                                 ((d) & IOPRIO_PRIO_MASK))

/* NUMA memory policies. AzamiOS presents a single memory node, so these are
 * fully implemented for the one-node case rather than stubbed: a policy is
 * stored and reported back faithfully, and any nodemask naming a node other
 * than 0 is rejected exactly as it would be on a one-node Linux box. */
#define MPOL_DEFAULT     0
#define MPOL_PREFERRED   1
#define MPOL_BIND        2
#define MPOL_INTERLEAVE  3
#define MPOL_LOCAL       4
#define MPOL_MAX         5
#define MPOL_F_STATIC_NODES   (1 << 15)
#define MPOL_F_RELATIVE_NODES (1 << 14)
#define MPOL_MODE_FLAGS  (MPOL_F_STATIC_NODES | MPOL_F_RELATIVE_NODES)
#define MPOL_F_NODE      (1 << 0)
#define MPOL_F_ADDR      (1 << 1)
#define MPOL_F_MEMS_ALLOWED (1 << 2)

/* futex2 (futex_wake/futex_wait/futex_requeue) flag word. */
#define FUTEX2_SIZE_U8    0x00
#define FUTEX2_SIZE_U16   0x01
#define FUTEX2_SIZE_U64   0x03
#define FUTEX2_NUMA       0x04
#define FUTEX2_SIZE_MASK  0x03

/* Linux statx timestamp and structure */
struct statx_timestamp {
    int64_t  tv_sec;
    uint32_t tv_nsec;
    int32_t  __reserved;
};

struct statx {
    uint32_t stx_mask;
    uint32_t stx_blksize;
    uint64_t stx_attributes;
    uint32_t stx_nlink;
    uint32_t stx_uid;
    uint32_t stx_gid;
    uint16_t stx_mode;
    uint16_t __spare0[1];
    uint64_t stx_ino;
    uint64_t stx_size;
    uint64_t stx_blocks;
    uint64_t stx_attributes_mask;
    struct statx_timestamp stx_atime;
    struct statx_timestamp stx_btime;
    struct statx_timestamp stx_ctime;
    struct statx_timestamp stx_mtime;
    uint32_t stx_rdev_major;
    uint32_t stx_rdev_minor;
    uint32_t stx_dev_major;
    uint32_t stx_dev_minor;
    uint64_t stx_mnt_id;
    uint64_t __spare2;
    uint64_t __spare3[12];
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

/* cachestat(2) — page-cache residency of a byte range of one file. */
struct cachestat_range {
    uint64_t off;
    uint64_t len;
};

struct cachestat {
    uint64_t nr_cache;             /* pages resident in the page cache          */
    uint64_t nr_dirty;             /* resident and dirty                        */
    uint64_t nr_writeback;         /* currently being written back              */
    uint64_t nr_evicted;           /* evicted since the file was last read      */
    uint64_t nr_recently_evicted;  /* evicted within the last refault window    */
};

/* futex_waitv(2) — wait on several futexes at once. */
struct futex_waitv {
    uint64_t val;
    uint64_t uaddr;
    uint32_t flags;
    uint32_t __reserved;
};

#define FUTEX2_SIZE_U32   0x02
#define FUTEX2_PRIVATE    0x80
#define FUTEX_WAITV_MAX   128

/* pkey_alloc(2) access-rights bits, mirrored into PKRU. */
#define PKEY_DISABLE_ACCESS  0x1
#define PKEY_DISABLE_WRITE   0x2
#define PKEY_ACCESS_MASK     (PKEY_DISABLE_ACCESS | PKEY_DISABLE_WRITE)
#define PKEY_MAX             16

/* POSIX *at flags */
#define AT_FDCWD            (-100)
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_REMOVEDIR        0x200
#define AT_EMPTY_PATH       0x1000
#define AT_STATX_SYNC_AS_STAT 0x0000
#define AT_STATX_FORCE_SYNC   0x2000
#define AT_STATX_DONT_SYNC    0x4000

/* utimensat(2)/futimens(2) tv_nsec sentinels. */
#define UTIME_NOW  ((1L << 30) - 1L)
#define UTIME_OMIT ((1L << 30) - 2L)

/* mlockall(2) / mlock2(2) flags. */
#define MCL_CURRENT     1
#define MCL_FUTURE      2
#define MCL_ONFAULT     4
#define MLOCK_ONFAULT   1

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
/* POSIX.1e ACL calls. These are an Azami extension with no Linux counterpart,
 * so they live in the private range: they previously sat on 328/329, which are
 * Linux's pwritev2 and pkey_mprotect, and whichever of the two colliding pairs
 * syscall_init() registered last silently won. */
#define SYS_AZ_GETFACL         532
#define SYS_AZ_SETFACL         533

/* Kept so existing callers keep building against the renumbered calls. */
#define SYS_getfacl       SYS_AZ_GETFACL
#define SYS_setfacl       SYS_AZ_SETFACL

/* ── POSIX Named Semaphores (Azami extended, numbers 534–541) ────────────── */
/* These numbers sit above the current Linux ABI (533) and will not collide
 * with future upstream additions for many years. The userland libc wrappers
 * use these when sem_open(3) / sem_post(3) etc. need the named-semaphore path;
 * the simpler sem_init()/sem_post()/sem_wait() spinlock path stays in-process. */
#define SYS_AZ_SEM_OPEN      534
#define SYS_AZ_SEM_CLOSE     535
#define SYS_AZ_SEM_POST      536
#define SYS_AZ_SEM_WAIT      537
#define SYS_AZ_SEM_TRYWAIT   538
#define SYS_AZ_SEM_TIMEDWAIT 539
#define SYS_AZ_SEM_UNLINK    540
#define SYS_AZ_SEM_GETVALUE  541

/* ── ktrace function tracer control (Azami extended, numbers 542–544) ──────── */
/* Enabled by writing to /sys/kernel/trace/enable; reading the ring buffer
 * from /sys/kernel/trace/pipe.  These syscalls are the VFS-less fallback. */
#define SYS_AZ_KTRACE_ENABLE  542
#define SYS_AZ_KTRACE_READ    543
#define SYS_AZ_KTRACE_CLEAR   544

/* Spawns like SYS_AZ_SPAWN, but with a second string (rsi) passed through as
 * argv[1] — e.g. a file path for a GUI app to open. rsi may be NULL/absent,
 * in which case behaviour is identical to SYS_AZ_SPAWN. Kept as a separate
 * number rather than overloading SYS_AZ_SPAWN's rsi so every existing
 * single-argument caller (built with syscall1(), which never constrains
 * rsi) keeps working unaffected by whatever garbage happens to sit in rsi
 * at their call site. */
#define SYS_AZ_SPAWN_ARG      545

/* ── System Telemetry (SYS_AZ_SYSSTAT) ──────────────────────────────────── */
typedef struct {
    uint64_t idle_ticks[16];
    uint64_t active_ticks[16];
} az_sysstat_t;

/* ── Framebuffer info structure (returned by SYS_AZ_FB_INFO) ───────────────── */
typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint8_t  bpp;
    uint8_t  _pad[3];
    uint64_t phys_addr;    /* Physical address of the framebuffer (for reference) */
} az_fb_info_t;
