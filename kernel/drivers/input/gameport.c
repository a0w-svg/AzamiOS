/**
 * gameport.c  –  Standard PC Analog Gameport Joystick Driver
 */
#include "./include/gameport.h"
#include "../klibc/include/port.h"
#include "../klibc/include/stdio.h"

static bool g_gameport_present = false;

void gameport_init(void) {
    kprintf("\ngameport: probing I/O port 0x%x for analog joystick axes...\n", GAMEPORT_IO_BASE);

    /* Read initial status */
    uint8_t status = inb(GAMEPORT_IO_BASE);
    if (status == 0xFF) {
        kprintf("gameport: no hardware detected at port 0x%x\n", GAMEPORT_IO_BASE);
        return;
    }

    g_gameport_present = true;
    kprintf("gameport: analog joystick interface ready (4 axes, 4 buttons active)\n");
}
