#ifndef PORT_H
#define PORT_H
#include <stdint.h>
/*
    read 1 byte data from port register;
*/
uint8_t inb(uint16_t port);
/*
    write 1 byte data to port register;
*/
void outb(uint16_t port, uint8_t data);
/*
    read 2 bytes data from port register;
*/
uint16_t inw(uint16_t port);
/*
    write 2 bytes data to port register;
*/
void outw(uint16_t port, uint16_t data);
/*
    read 4 bytes from port register;
*/
uint32_t inl(uint16_t port);
/*
    write 4 bytes to port register;
*/
void outl(uint16_t port, uint32_t data);
#endif