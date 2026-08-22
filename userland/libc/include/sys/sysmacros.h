/* ============================================================================
 * AzamiOS Userspace — Device Number Macros (sys/sysmacros.h)
 * File: userland/libc/include/sys/sysmacros.h
 * ============================================================================ */
#pragma once

#include "types.h"

#define major(dev)       ((unsigned int)(((dev) >> 8) & 0xfff))
#define minor(dev)       ((unsigned int)((dev) & 0xff))
#define makedev(ma, mi)  (((unsigned long)((ma) & 0xfff) << 8) | (unsigned long)((mi) & 0xff))
