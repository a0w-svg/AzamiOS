/**
 * kernel/filesystem/include/vfs.h — Kernel VFS header bridge
 *
 * The canonical definitions live in lib/fs/vfs.h.
 * This file re-exports them so that existing kernel includes are
 * unaffected (no path changes needed in fat32.c, tarfs.c, syscall.c, …).
 */
#include "../../../lib/fs/vfs.h"

/* Kernel-only extension: called at boot to register kprintf as VFS log sink */
void vfs_kernel_init(void);