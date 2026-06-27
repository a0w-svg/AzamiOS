/**
 * kernel/klibc/string.c — Kernel string/memory bridge
 *
 * The canonical implementation now lives in lib/string/string.c.
 * This file includes it directly so the kernel gets the same portable
 * code without a separate compilation unit or linker step.
 *
 * To compile the canonical lib independently (host gcc unit tests):
 *   gcc -ffreestanding -o test_string lib/string/string.c test_string.c
 */
#include "../../lib/string/string.c"
