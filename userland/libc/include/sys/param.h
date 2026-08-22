/* ============================================================================
 * AzamiOS Userspace — System Parameters (sys/param.h)
 * File: userland/libc/include/sys/param.h
 * ============================================================================ */
#pragma once

#include "types.h"

#define MAXPATHLEN  4096
#define MAXHOSTNAMELEN 64

#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif

#ifndef howmany
#define howmany(x, y) (((x) + ((y) - 1)) / (y))
#endif

#ifndef rounddown
#define rounddown(x, y) (((x) / (y)) * (y))
#endif

#ifndef roundup
#define roundup(x, y) ((((x) + ((y) - 1)) / (y)) * (y))
#endif
