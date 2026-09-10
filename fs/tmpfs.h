/* ============================================================================
 * AzamiOS — tmpfs: RAM-backed volatile filesystem
 * File: fs/tmpfs.h
 *
 * tmpfs stores inodes and file data entirely in kernel heap memory (kmalloc).
 * It is mounted at /tmp during kernel boot and disappears on reboot.
 * ============================================================================ */
#pragma once

#include "../include/azami/types.h"

struct inode;

/**
 * tmpfs_init() — Register the "tmpfs" filesystem type with the VFS layer.
 * Must be called after vfs_init() and before vfs_mount() for /tmp.
 */
void tmpfs_init(void);

/**
 * tmpfs_truncate() — Truncate or extend a tmpfs file to length bytes.
 */
s64 tmpfs_truncate(struct inode *inode, u64 length);
