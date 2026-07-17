/**
 * include/port.h  –  AzamiOS Ring-3 Userspace Port I/O Driver Primitives
 */
#ifndef _USER_PORT_H
#define _USER_PORT_H

#include <stdint.h>

uint8_t sys_inb(uint16_t port);
void sys_outb(uint16_t port, uint8_t data);
uint16_t sys_inw(uint16_t port);
void sys_outw(uint16_t port, uint16_t data);
uint32_t sys_inl(uint16_t port);
void sys_outl(uint16_t port, uint32_t data);

#endif /* _USER_PORT_H */
