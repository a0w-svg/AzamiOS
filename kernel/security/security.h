/* ============================================================================
 * AzamiOS — Security Hardening & Canary Subsystem Header
 * File: kernel/security/security.h
 * ============================================================================ */
#pragma once

#include "../../include/azami/types.h"
#include "../sched/sched.h"

extern uintptr_t __stack_chk_guard;

/** security_init() — Initialize stack canaries and security boundaries. */
void security_init(void);

/** security_validate_user_ptr(ptr, size) — Verify pointer is within ring-3 space. */
bool security_validate_user_ptr(const void *ptr, size_t size);

/** security_validate_kernel_ptr(ptr, size) — Verify pointer is in higher-half space. */
bool security_validate_kernel_ptr(const void *ptr, size_t size);

/** security_format_status(buf, max) — Render the boot-time hardening posture
 *  (SMEP/SMAP/UMIP/NX/WP, canary quality, MCA, split-lock, CET availability)
 *  as one "name: value" line per item. Backs /proc/security. */
size_t security_format_status(char *buf, size_t max);

/* Standard POSIX / Linux Capabilities (values match Linux's <linux/capability.h>
 * so userland headers and capget/capset payloads line up). */
#define CAP_CHOWN            0
#define CAP_DAC_OVERRIDE     1
#define CAP_DAC_READ_SEARCH  2
#define CAP_FOWNER           3
#define CAP_FSETID           4
#define CAP_KILL             5
#define CAP_SETGID           6
#define CAP_SETUID           7
#define CAP_SETPCAP          8
#define CAP_LINUX_IMMUTABLE  9
#define CAP_NET_BIND_SERVICE 10
#define CAP_NET_BROADCAST    11
#define CAP_NET_ADMIN        12
#define CAP_NET_RAW          13
#define CAP_IPC_LOCK         14
#define CAP_IPC_OWNER        15
#define CAP_SYS_MODULE       16
#define CAP_SYS_RAWIO        17
#define CAP_SYS_CHROOT       18
#define CAP_SYS_PTRACE       19
#define CAP_SYS_PACCT        20
#define CAP_SYS_ADMIN        21
#define CAP_SYS_BOOT         22
#define CAP_SYS_NICE         23
#define CAP_SYS_RESOURCE     24
#define CAP_SYS_TIME         25
#define CAP_SYS_TTY_CONFIG   26
#define CAP_MKNOD            27
#define CAP_LEASE            28
#define CAP_AUDIT_WRITE      29
#define CAP_AUDIT_CONTROL    30
#define CAP_SETFCAP          31
#define CAP_MAC_OVERRIDE     32
#define CAP_MAC_ADMIN        33
#define CAP_SYSLOG           34
#define CAP_WAKE_ALARM       35
#define CAP_BLOCK_SUSPEND    36
#define CAP_AUDIT_READ       37
#define CAP_PERFMON          38
#define CAP_BPF              39
#define CAP_CHECKPOINT_RESTORE 40

#define CAP_LAST_CAP         CAP_CHECKPOINT_RESTORE

/* Every defined capability. Bits above CAP_LAST_CAP are always rejected so a
 * capset() payload cannot park bits in the set that a future CAP_* would
 * silently inherit as already-granted. */
#define CAP_FULL_SET   ((CAP_LAST_CAP >= 63) ? ~0ULL \
                        : ((1ULL << (CAP_LAST_CAP + 1)) - 1))

#define CAP_TO_MASK(c) (1ULL << (c))

/* seccomp(2) operations / modes we implement. */
#define SECCOMP_MODE_DISABLED       0
#define SECCOMP_MODE_STRICT         1
#define SECCOMP_MODE_FILTER         2
#define SECCOMP_SET_MODE_STRICT     0
#define SECCOMP_SET_MODE_FILTER     1
#define SECCOMP_GET_ACTION_AVAIL    2

/* SECCOMP_MODE_FILTER: classic-BPF program attachment. Only the flag AzamiOS
 * actually honours — none, i.e. flags must be 0 — is listed; SECCOMP_FILTER_
 * FLAG_TSYNC and friends exist in Linux for multi-threaded thread-group
 * semantics this kernel's process model doesn't have. */
#define SECCOMP_FILTER_FLAG_NONE    0

/* Classic-BPF program a filter is built from, and what seccomp(2) /
 * prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, ...) take a pointer to — same
 * layout as Linux's struct sock_filter / struct sock_fprog so a filter built
 * with the standard BPF_STMT/BPF_JUMP macros (userland: <linux/filter.h>)
 * needs no AzamiOS-specific changes. */
typedef struct {
    u16 code;
    u8  jt;
    u8  jf;
    u32 k;
} sock_filter_t;

typedef struct {
    u16 len;                 /* number of instructions in `filter` */
    sock_filter_t *filter;   /* user pointer */
} sock_fprog_t;

/* What the BPF program is run against — same field layout/offsets as
 * Linux's struct seccomp_data, since BPF_LD+BPF_ABS instructions address it
 * by byte offset and a real seccomp-BPF program's offsets are baked in at
 * compile time. */
typedef struct {
    s32 nr;
    u32 arch;
    u64 instruction_pointer;
    u64 args[6];
} seccomp_data_t;

#define AUDIT_ARCH_X86_64  0xC000003EU

/* SECCOMP_RET_*: what a filter's return value means, and the two masks that
 * split it into an action (top 16 bits, SECCOMP_RET_KILL_PROCESS excepted —
 * see seccomp.c) and per-action data (bottom 16 bits, the errno for
 * SECCOMP_RET_ERRNO). Values match Linux's <linux/seccomp.h> so a filter
 * program built against the real headers returns the same things here. */
#define SECCOMP_RET_KILL_PROCESS   0x80000000U
#define SECCOMP_RET_KILL_THREAD    0x00000000U
#define SECCOMP_RET_KILL           SECCOMP_RET_KILL_THREAD
#define SECCOMP_RET_TRAP           0x00030000U
#define SECCOMP_RET_ERRNO          0x00050000U
#define SECCOMP_RET_TRACE          0x7ff00000U
#define SECCOMP_RET_LOG            0x7ffc0000U
#define SECCOMP_RET_ALLOW          0x7fff0000U
#define SECCOMP_RET_ACTION_FULL    0xffff0000U
#define SECCOMP_RET_DATA           0x0000ffffU

/** seccomp_attach_filter(proc, ufprog) — seccomp(SECCOMP_SET_MODE_FILTER, ...)
 *  and prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, ...) both land here: copy
 *  the program in from `ufprog` (a user struct sock_fprog *), validate it,
 *  and prepend it to @proc's filter chain. Returns 0, or a negative errno
 *  (-EINVAL: bad program; -EFAULT: program unreadable; -ENOMEM). Sets
 *  @proc->seccomp_mode and @proc->no_new_privs on success, the latter for
 *  the same reason SECCOMP_SET_MODE_STRICT does (security.c). */
s64 seccomp_attach_filter(process_t *proc, const sock_fprog_t *ufprog);

/** seccomp_filter_run(proc, syscall_nr, regs) — Run every filter attached to
 *  @proc against this syscall (newest-attached first) and return the most
 *  restrictive SECCOMP_RET_* action across all of them, exactly as Linux's
 *  seccomp_run_filters() does for a stacked set of filters. Only meaningful
 *  when proc->seccomp_mode == SECCOMP_MODE_FILTER. */
u32 seccomp_filter_run(process_t *proc, u64 syscall_nr, const pt_regs_t *regs);

/** seccomp_filters_share(child, parent) — fork(): the child starts out
 *  sharing the parent's filter chain (one more reference on the shared
 *  head), exactly like Linux. A later seccomp() call in either process
 *  prepends a new filter without disturbing what the other one sees. */
void seccomp_filters_share(process_t *child, const process_t *parent);

/** seccomp_filters_put(proc) — Drop this process's reference to its filter
 *  chain. Called once from proc_destroy(); safe to call with no filters
 *  attached (proc->seccomp_filters == NULL). */
void seccomp_filters_put(process_t *proc);

/** security_check_permission(proc, capability) — Check capability bits on a process. */
bool security_check_permission(process_t *proc, u32 capability);

/** security_caps_init(proc, is_first) — Seed a brand-new process's capability
 *  sets. `is_first` marks pid<=1 (kernel/init), which starts fully privileged. */
void security_caps_init(process_t *proc, bool is_first);

/** security_caps_on_setuid(proc) — Recompute the effective/permitted sets after
 *  a credential change, applying the traditional "root holds every capability"
 *  rule while keeping the bounding set as a hard ceiling. */
void security_caps_on_setuid(process_t *proc);

/** security_caps_on_exec(proc) — Apply the execve() capability transition. */
void security_caps_on_exec(process_t *proc);

/** security_seccomp_check(proc, syscall_nr) — Returns true when the syscall is
 *  permitted under the process's seccomp mode. */
bool security_seccomp_check(process_t *proc, u64 syscall_nr);

/* Security knobs / sysctls */
extern u64 g_mmap_min_addr;
extern u32 g_dmesg_restrict;
extern u32 g_kptr_restrict;
extern u32 g_yama_ptrace_scope;
extern u32 g_protected_hardlinks;
extern u32 g_protected_symlinks;
