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
#define SECCOMP_SET_MODE_STRICT     0
#define SECCOMP_SET_MODE_FILTER     1
#define SECCOMP_GET_ACTION_AVAIL    2

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
