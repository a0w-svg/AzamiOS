/* ============================================================================
 * AzamiOS — Power Management & ACPI System Control Driver
 * File: drivers/acpi/power.c
 * ============================================================================ */

#define DEBUG 1
#include <azami/debug.h>
#include "power.h"
#include "../../include/azami/defs.h"
#include "../../drivers/char/console.h"

__attribute__((noreturn)) void power_shutdown(void)
{
    pr_debug("[POWER] Powering down system...\n");

    /* 1. Try QEMU 0x604 ACPI poweroff */
    outw(0x604, 0x2000);

    /* 2. Try Bochs / older QEMU port */
    outw(0xB004, 0x2000);

    /* 3. Try VirtualBox poweroff */
    outw(0x4004, 0x3400);

    /* 4. Try APM shutdown */
    outw(0x8900, 0x5307);

    pr_debug("[POWER] System halted. It is now safe to turn off your computer.\n");
    cpu_halt_loop();
    __builtin_unreachable();
}

__attribute__((noreturn)) void power_reboot(void)
{
    pr_debug("[POWER] Rebooting system...\n");

    /* 1. Try 8042 PS/2 controller pulse reset */
    u8 temp;
    do {
        temp = inb(0x64);
        if (temp & 1) (void)inb(0x60);
    } while (temp & 2);

    outb(0x64, 0xFE);

    /* 2. Try PCI reset register 0xCF9 */
    outb(0xCF9, 0x02);
    outb(0xCF9, 0x06);

    /* 3. Fallback: Triple fault via invalid IDT */
    struct {
        u16 limit;
        u64 base;
    } __attribute__((packed)) null_idt = { 0, 0 };
    __asm__ volatile("lidt %0; int3" : : "m"(null_idt));

    cpu_halt_loop();
    __builtin_unreachable();
}
