/**
 * include/gameport.h  –  AzamiOS Ring-3 Gameport Joystick Driver Header
 */
#ifndef _USER_GAMEPORT_H
#define _USER_GAMEPORT_H

#include <stdbool.h>
#include <stdint.h>

#define GAMEPORT_IO_BASE 0x201

bool gameport_probe(void);
uint8_t gameport_read_status(void);

#endif /* _USER_GAMEPORT_H */
