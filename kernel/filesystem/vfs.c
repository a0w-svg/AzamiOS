/**
 * kernel/filesystem/vfs.c — Kernel VFS bridge
 *
 * The canonical implementation lives in lib/fs/vfs.c.
 * This file includes it, then registers kprintf as the VFS log callback
 * so that error messages appear on the kernel console.
 */
#include "../../lib/fs/vfs.c"
#include "../klibc/include/stdio.h"

/* Wire up kprintf as the VFS log sink at early boot (called from x86arch.c). */
void vfs_kernel_init(void) {
    vfs_set_log(kprintf);
}