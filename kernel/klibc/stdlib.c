/**
 * kernel/klibc/stdlib.c — Kernel stdlib bridge
 *
 * The canonical implementation now lives in lib/stdlib/stdlib.c.
 * This file includes it directly so the kernel gets the same portable
 * code without a separate compilation unit.
 */
#include "../../lib/stdlib/stdlib.c"
