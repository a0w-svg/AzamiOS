/* ============================================================================
 * AzamiOS Userspace — POSIX Error Numbers (errno.h)
 * File: userland/libc/include/errno.h
 * ============================================================================ */
#pragma once

extern int errno;

/* Canonical E* values, shared verbatim with the kernel
 * (include/azami/defs.h). This copy is regenerated from
 * ../../../include/azami/uapi/errno.h by userland/Makefile's uapi-sync
 * target on every build — never hand-edit it or add an E* define anywhere
 * else. See scripts/check_uapi_sync.sh. */
#include "azami/uapi/errno.h"
