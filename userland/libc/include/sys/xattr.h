/* ============================================================================
 * AzamiOS Userspace — Linux Extended Attributes Header (sys/xattr.h)
 * File: userland/libc/include/sys/xattr.h
 * ============================================================================ */
#pragma once

#include <sys/types.h>

#define XATTR_CREATE  0x1 /* set value, fail if attr already exists */
#define XATTR_REPLACE 0x2 /* set value, fail if attr does not exist */

ssize_t getxattr(const char *path, const char *name, void *value, size_t size);
ssize_t lgetxattr(const char *path, const char *name, void *value, size_t size);
ssize_t fgetxattr(int fd, const char *name, void *value, size_t size);

int setxattr(const char *path, const char *name, const void *value, size_t size, int flags);
int lsetxattr(const char *path, const char *name, const void *value, size_t size, int flags);
int fsetxattr(int fd, const char *name, const void *value, size_t size, int flags);

ssize_t listxattr(const char *path, char *list, size_t size);
ssize_t llistxattr(const char *path, char *list, size_t size);
ssize_t flistxattr(int fd, char *list, size_t size);

int removexattr(const char *path, const char *name);
int lremovexattr(const char *path, const char *name);
int fremovexattr(int fd, const char *name);
