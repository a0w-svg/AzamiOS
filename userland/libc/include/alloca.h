/* ============================================================================
 * AzamiOS Userspace — Stack Memory Allocation (alloca.h)
 * File: userland/libc/include/alloca.h
 * ============================================================================ */
#pragma once

#include <stddef.h>

#undef alloca
#define alloca(size) __builtin_alloca(size)
