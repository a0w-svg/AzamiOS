/* ============================================================================
 * AzamiOS — lock: Lock Current Session
 * File: userland/apps/lock/main.c
 * ============================================================================ */

#include <stdio.h>
#include <unistd.h>
#include "../../libc/include/az/ipc.h"

int main(void)
{
    puts("[lock] Locking AzamiOS desktop session...");
    int pid = az_spawn("/sbin/lockscreen.elf");
    if (pid < 0) {
        fprintf(stderr, "lock: failed to launch /sbin/lockscreen.elf\n");
        return 1;
    }
    return 0;
}
