/**
 * kernel/filesystem/vfs_dev.c — Kernel VFS Device Node bridge
 *
 * Pulls in the kernel-independent lib/fs/vfs_dev.c implementation
 * so it is compiled once as part of the kernel build.
 */
#include "../../lib/fs/vfs_dev.c"
