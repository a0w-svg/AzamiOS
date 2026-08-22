/* ============================================================================
 * AzamiOS Userspace — System Init Daemon (init.elf - PID 1)
 * File: user/apps/init/main.c
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/unistd.h"
#include "../../libc/include/sys/syscall.h"
#include "../../libc/include/az/ipc.h"

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    puts("===============================================================================");
    puts("             AzamiOS v7.0 — Modular Ring 3 Userspace (init.elf)");
    puts("===============================================================================");
    puts("[init] System initialization process spawned successfully (PID 1).");
    puts("[init] Spawning session manager (sessiond.elf)...");

    /* ── Spawn Session Manager Daemon ────────────────────────────────────── */
    int pid = az_spawn("/sbin/sessiond.elf");
    if (pid < 0) {
        puts("[init] ERROR: az_spawn /sbin/sessiond.elf failed! Attempting direct fallback...");
        az_spawn("/sbin/azwm.elf");
        for (int i = 0; i < 100; i++) az_yield();
        az_spawn("/sbin/wallpaper.elf");
        az_spawn("/sbin/taskbar.elf");
    } else {
        puts("[init] sessiond.elf spawned successfully.");
    }

    /* ── PID 1 Idle & Zombie Reaper Loop ─────────────────────────────────── */
    for (;;) {
        sleep(1);
    }

    return 0;
}

