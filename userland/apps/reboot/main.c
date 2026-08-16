/* ============================================================================
 * AzamiOS Userspace — System Reboot Utility (reboot.elf)
 * File: userland/apps/reboot/main.c
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/sys/reboot.h"
#include "../../libc/include/unistd.h"

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("[reboot] Requesting system hardware reset...\n");
    reboot(RB_AUTOBOOT);
    return 0;
}
