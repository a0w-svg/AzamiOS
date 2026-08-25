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

/* Standard POSIX / Linux Capabilities */
#define CAP_CHOWN            0
#define CAP_DAC_OVERRIDE     1
#define CAP_DAC_READ_SEARCH  2
#define CAP_FOWNER           3
#define CAP_KILL             5
#define CAP_SETGID           6
#define CAP_SETUID           7
#define CAP_NET_BIND_SERVICE 10
#define CAP_NET_ADMIN        12
#define CAP_NET_RAW          13
#define CAP_SYS_CHROOT       18
#define CAP_SYS_ADMIN        21
#define CAP_SYS_BOOT         22
#define CAP_SYS_TIME         25

/** security_check_permission(proc, capability) — Check capability bits on a process. */
bool security_check_permission(process_t *proc, u32 capability);
