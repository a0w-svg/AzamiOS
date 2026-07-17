/**
 * gameport.c  –  AzamiOS Ring-3 Gameport Joystick Driver Implementation
 */
#include "gameport.h"
#include "port.h"


bool gameport_probe(void) {
    uint8_t status = sys_inb(GAMEPORT_IO_BASE);
    return (status != 0xFF);
}

uint8_t gameport_read_status(void) {
    return sys_inb(GAMEPORT_IO_BASE);
}
