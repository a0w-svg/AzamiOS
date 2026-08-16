/* ============================================================================
 * AzamiOS Userspace — System Poweroff Utility (poweroff.elf)
 * File: userland/apps/poweroff/main.c
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/sys/reboot.h"
#include "../../libc/include/unistd.h"

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("[poweroff] Requesting system ACPI poweroff...\n");
    reboot(RB_POWER_OFF);
    return 0;
}
