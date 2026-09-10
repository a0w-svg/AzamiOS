/* ============================================================================
 * AzamiOS Userspace — x86_64 Port I/O & Permissions Header (sys/io.h)
 * File: userland/libc/include/sys/io.h
 * ============================================================================ */
#pragma once

#include "syscall.h"
#include "../errno.h"

static inline unsigned char inb(unsigned short port)
{
    unsigned char val;
    __asm__ volatile("inb %w1, %b0" : "=a"(val) : "Nd"(port));
    return val;
}

static inline void outb(unsigned char val, unsigned short port)
{
    __asm__ volatile("outb %b0, %w1" : : "a"(val), "Nd"(port));
}

static inline unsigned short inw(unsigned short port)
{
    unsigned short val;
    __asm__ volatile("inw %w1, %w0" : "=a"(val) : "Nd"(port));
    return val;
}

static inline void outw(unsigned short val, unsigned short port)
{
    __asm__ volatile("outw %w0, %w1" : : "a"(val), "Nd"(port));
}

static inline unsigned int inl(unsigned short port)
{
    unsigned int val;
    __asm__ volatile("inl %w1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static inline void outl(unsigned int val, unsigned short port)
{
    __asm__ volatile("outl %0, %w1" : : "a"(val), "Nd"(port));
}

static inline unsigned char inb_p(unsigned short port)
{
    unsigned char val = inb(port);
    __asm__ volatile("outb %%al, $0x80" : : : "memory");
    return val;
}

static inline void outb_p(unsigned char val, unsigned short port)
{
    outb(val, port);
    __asm__ volatile("outb %%al, $0x80" : : : "memory");
}

static inline unsigned short inw_p(unsigned short port)
{
    unsigned short val = inw(port);
    __asm__ volatile("outb %%al, $0x80" : : : "memory");
    return val;
}

static inline void outw_p(unsigned short val, unsigned short port)
{
    outw(val, port);
    __asm__ volatile("outb %%al, $0x80" : : : "memory");
}

static inline unsigned int inl_p(unsigned short port)
{
    unsigned int val = inl(port);
    __asm__ volatile("outb %%al, $0x80" : : : "memory");
    return val;
}

static inline void outl_p(unsigned int val, unsigned short port)
{
    outl(val, port);
    __asm__ volatile("outb %%al, $0x80" : : : "memory");
}

static inline int iopl(int level)
{
    long ret = syscall1(SYS_iopl, level);
    if (ret < 0) {
        errno = (int)-ret;
        return -1;
    }
    return 0;
}

static inline int ioperm(unsigned long from, unsigned long num, int turn_on)
{
    long ret = syscall3(SYS_ioperm, (long)from, (long)num, turn_on);
    if (ret < 0) {
        errno = (int)-ret;
        return -1;
    }
    return 0;
}
