/**
 * kernel/filesystem/tarfs.c — Kernel tarfs bridge
 *
 * The canonical implementation lives in lib/fs/tarfs.c.
 * This file includes it directly so the kernel gets the same portable
 * code without a separate compilation unit.
 */
#include "../../lib/fs/tarfs.c"
