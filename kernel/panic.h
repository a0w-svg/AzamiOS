/* ============================================================================
 * AzamiOS — Kernel Panic Header
 * File: kernel/panic.h
 * ============================================================================ */
#pragma once
#include "../include/azami/defs.h"
#include <stdarg.h>

/* kernel_panic is already declared in defs.h via the PANIC macro, but we
 * also declare it here for files that include panic.h directly. */
__noreturn void kernel_panic(const char *fmt, ...);
