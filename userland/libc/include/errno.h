/* ============================================================================
 * AzamiOS Userspace — POSIX Error Numbers (errno.h)
 * File: userland/libc/include/errno.h
 * ============================================================================ */
#pragma once

/* Per-thread since Phase 1 of the libc hardening work: each thread (main or
 * pthread_create()'d) gets its own %fs-relative storage — see
 * userland/libc/tls.c and thread_startup_trampoline() in pthread.c. */
extern __thread int errno;

/* Canonical E* values, shared verbatim with the kernel
 * (include/azami/defs.h). This copy is regenerated from
 * ../../../include/azami/uapi/errno.h by userland/Makefile's uapi-sync
 * target on every build — never hand-edit it or add an E* define anywhere
 * else. See scripts/check_uapi_sync.sh. */
#include "azami/uapi/errno.h"
