/**
 * port.c  –  AzamiOS Ring-3 Userspace Port I/O Driver Implementation
 */
#include "port.h"

#include <sys/syscall.h>

uint8_t sys_inb(uint16_t port) {
    uint32_t ret;
    asm volatile("int $128" : "=a"(ret) : "a"(SYS_INB), "b"(port) : "memory");
    return (uint8_t)ret;
}

void sys_outb(uint16_t port, uint8_t data) {
    asm volatile("int $128" : : "a"(SYS_OUTB), "b"(port), "c"(data) : "memory");
}

uint16_t sys_inw(uint16_t port) {
    uint32_t ret;
    asm volatile("int $128" : "=a"(ret) : "a"(SYS_INW), "b"(port) : "memory");
    return (uint16_t)ret;
}

void sys_outw(uint16_t port, uint16_t data) {
    asm volatile("int $128" : : "a"(SYS_OUTW), "b"(port), "c"(data) : "memory");
}

uint32_t sys_inl(uint16_t port) {
    uint32_t ret;
    asm volatile("int $128" : "=a"(ret) : "a"(SYS_INL), "b"(port) : "memory");
    return ret;
}

void sys_outl(uint16_t port, uint32_t data) {
    asm volatile("int $128" : : "a"(SYS_OUTL), "b"(port), "c"(data) : "memory");
}
