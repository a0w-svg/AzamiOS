/* ============================================================================
 * AzamiOS Userspace — Zero-Copy Data Transfer Interface (sys/sendfile.h)
 * File: userland/libc/include/sys/sendfile.h
 * ============================================================================ */
#pragma once

#include "types.h"

ssize_t sendfile(int out_fd, int in_fd, off_t *offset, size_t count);
