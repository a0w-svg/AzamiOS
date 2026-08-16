/* ============================================================================
 * AzamiOS — Process Filesystem (procfs) Header
 * File: fs/procfs.h
 * ============================================================================ */
#pragma once

#include "vfs.h"

/** procfs_init() — Register the procfs filesystem type in VFS. */
void procfs_init(void);
