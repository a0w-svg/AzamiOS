/* ============================================================================
 * AzamiOS Userspace — Kernel Random Number Generator Interface (sys/random.h)
 * File: userland/libc/include/sys/random.h
 * ============================================================================ */
#pragma once

#include "types.h"

#define GRND_NONBLOCK 0x0001
#define GRND_RANDOM   0x0002

ssize_t getrandom(void *buf, size_t buflen, unsigned int flags);
int     getentropy(void *buffer, size_t length);
