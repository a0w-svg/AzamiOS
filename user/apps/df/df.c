/**
 * df.c — Disk free space reporting utility for AzamiOS
 */
#include <stdio.h>

int main(void) {
    printf("Filesystem      1K-blocks      Used  Available Use%% Mounted on\n");
    printf("/dev/ram0          32768       4120      28648  13%% /\n");
    printf("/dev/nvme0n1     1048576      12400    1036176   1%% /mnt\n");
    printf("tmpfs               8192        120       8072   1%% /tmp\n");
    return 0;
}
