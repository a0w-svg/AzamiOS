/* ============================================================================
 * AzamiOS Userspace — I/O Scheduling Priority (sys/ioprio.h)
 * File: userland/libc/include/sys/ioprio.h
 *
 * ioprio_set(2) / ioprio_get(2). Linux ships no libc wrappers for these, so
 * portable code calls them through syscall(); the declarations here save that
 * and, more usefully, put the class/level packing macros somewhere findable.
 *
 * A priority is one int: a three-bit class in the top bits and a level within
 * the class in the low thirteen. Build one with IOPRIO_PRIO_VALUE():
 *
 *     ioprio_set(IOPRIO_WHO_PROCESS, 0,
 *                IOPRIO_PRIO_VALUE(IOPRIO_CLASS_IDLE, 0));
 *
 * Only IOPRIO_CLASS_RT requires privilege — it can starve every other process
 * on the machine.
 * ============================================================================ */
#pragma once

#include "types.h"
#include "syscall.h" /* IOPRIO_* class/level packing macros — canonical uapi */

#ifdef __cplusplus
extern "C" {
#endif

/* IOPRIO_WHO_*, IOPRIO_CLASS_*, IOPRIO_CLASS_SHIFT, IOPRIO_PRIO_MASK,
 * IOPRIO_NR_LEVELS, and the IOPRIO_PRIO_CLASS/DATA/VALUE packing macros all
 * come from <sys/syscall.h>'s canonical uapi header
 * (include/azami/uapi/syscall_nr.h) — this used to be a second,
 * hand-duplicated copy of all of them, which is exactly the kind of drift
 * scripts/check_uapi_sync.sh exists to catch. */

/**
 * ioprio_set() — set the I/O priority of a process, process group, or user.
 * Returns 0, or -1 with errno set (EPERM for an unprivileged RT request,
 * EINVAL for a malformed class/level, ESRCH when nothing matched).
 */
int ioprio_set(int which, int who, int ioprio);

/**
 * ioprio_get() — read a priority back. Over a group or a user it reports the
 * *highest* priority found, which is the numerically smallest value. Returns
 * the priority, or -1 with errno set.
 */
int ioprio_get(int which, int who);

#ifdef __cplusplus
}
#endif
