/* ============================================================================
 * AzamiOS Userspace — Device Control Header (sys/ioctl.h)
 * File: userland/libc/include/sys/ioctl.h
 * ============================================================================ */
#pragma once

int ioctl(int fd, unsigned long request, ...);
