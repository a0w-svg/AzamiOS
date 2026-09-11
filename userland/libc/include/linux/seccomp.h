/* ============================================================================
 * AzamiOS — <linux/seccomp.h> compatibility subset
 *
 * Matches kernel/security/security.h's SECCOMP_* constants (same values, so
 * a filter's SECCOMP_RET_* return values mean the same thing on both sides
 * of the syscall boundary) and struct seccomp_data (kernel/security/
 * security.h: seccomp_data_t) — the BPF_LD|BPF_ABS offsets a filter program
 * reads are field offsets into this struct.
 * ============================================================================ */
#ifndef _LINUX_SECCOMP_H
#define _LINUX_SECCOMP_H

#include <stdint.h>

#define SECCOMP_MODE_DISABLED  0
#define SECCOMP_MODE_STRICT    1
#define SECCOMP_MODE_FILTER    2

#define SECCOMP_SET_MODE_STRICT   0
#define SECCOMP_SET_MODE_FILTER   1
#define SECCOMP_GET_ACTION_AVAIL  2

#define SECCOMP_FILTER_FLAG_NONE  0

#define SECCOMP_RET_KILL_PROCESS  0x80000000U
#define SECCOMP_RET_KILL_THREAD   0x00000000U
#define SECCOMP_RET_KILL          SECCOMP_RET_KILL_THREAD
#define SECCOMP_RET_TRAP          0x00030000U
#define SECCOMP_RET_ERRNO         0x00050000U
#define SECCOMP_RET_TRACE         0x7ff00000U   /* not enforced — see below */
#define SECCOMP_RET_LOG           0x7ffc0000U
#define SECCOMP_RET_ALLOW         0x7fff0000U

#define SECCOMP_RET_ACTION_FULL   0xffff0000U
#define SECCOMP_RET_DATA          0x0000ffffU

/* AzamiOS does not implement SECCOMP_RET_TRACE's ptrace-notify semantics
 * (PTRACE_EVENT_SECCOMP): a filter that returns it gets a syscall that
 * fails with -ENOSYS instead, and SECCOMP_GET_ACTION_AVAIL reports it
 * unavailable. Every other action here is fully enforced. */

#define AUDIT_ARCH_X86_64  0xC000003EU

struct seccomp_data {
    int      nr;
    uint32_t arch;
    uint64_t instruction_pointer;
    uint64_t args[6];
};

#endif /* _LINUX_SECCOMP_H */
