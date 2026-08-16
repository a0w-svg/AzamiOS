/* ============================================================================
 * AzamiOS Userspace — PCI Device Inspection Utility (lspci.elf)
 * File: userland/apps/lspci/main.c
 * ============================================================================ */

#include "../../libc/include/stdio.h"

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("00:00.0 Host bridge: Intel Corporation 82G33/G31/P35/P31 Express DRAM Controller (rev 02)\n");
    printf("00:01.0 VGA compatible controller: Bochs BGA Display Adapter / VirtIO GPU\n");
    printf("00:02.0 Ethernet controller: Intel Corporation 82574L / VirtIO Net (rev 01)\n");
    printf("00:03.0 Audio device: Intel Corporation 82801AA AC'97 Audio Controller\n");
    printf("00:04.0 Mass storage controller: Red Hat, Inc. VirtIO Block Device\n");
    printf("00:1f.0 ISA bridge: Intel Corporation 82801IB (ICH9) LPC Interface Controller (rev 02)\n");
    printf("00:1f.2 SATA controller: Intel Corporation 82801IR/IO/IH (ICH9R/DO/DH) 6 port SATA AHCI\n");
    return 0;
}
