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

/** security_check_permission(proc, capability) — Check capability bits on a process. */
bool security_check_permission(process_t *proc, u32 capability);
