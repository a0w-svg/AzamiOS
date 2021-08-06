#include "./include/port.h"

/*
    read 1 byte data from port register;
*/
uint8_t inb(uint16_t port)
{
    uint8_t result;
    __asm__ volatile("inb %%dx, %%al" : "=a" (result) : "d" (port));
    return result;
}

/*
    write 1 byte data to port register;
*/
void outb(uint16_t port, uint8_t data)
{
    __asm__ volatile("outb %%al, %%dx" : : "a" (data), "d" (port));
}

/*
    read 2 bytes data from port register;
*/
uint16_t inw(uint16_t port)
{
    uint16_t result;
    __asm__ volatile("inw %%dx, %%ax" : "=a" (result) : "d" (port));
    return result;
}

/*
    write 2 bytes data to port register;
*/
void outw(uint16_t port, uint16_t data)
{
     __asm__ volatile("outw %%ax, %%dx" : : "a" (data), "d" (port));
}

/*
    read 4 bytes from port register;
*/
uint32_t inl(uint16_t port)
{
    uint32_t result;
    __asm__ volatile("inl %1, %0" : "=a" (result) : "d" (port));
    return result;
}

/*
    write 4 bytes to port register;
*/
void outl(uint16_t port, uint32_t data)
{
     __asm__ volatile("outl %0, %1" : : "a" (data), "d" (port));
}