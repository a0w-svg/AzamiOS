#pragma once
#include "types.h"
#include "defs.h"  /* has inb/outb etc */

/* MMIO accessors (volatile, prevents compiler reordering) */
static inline u8  readb(const volatile void *addr) { return *(const volatile u8 *)addr; }
static inline u16 readw(const volatile void *addr) { return *(const volatile u16 *)addr; }
static inline u32 readl(const volatile void *addr) { return *(const volatile u32 *)addr; }
static inline u64 readq(const volatile void *addr) { return *(const volatile u64 *)addr; }

static inline void writeb(u8  val, volatile void *addr) { *(volatile u8 *)addr  = val; }
static inline void writew(u16 val, volatile void *addr) { *(volatile u16 *)addr = val; }
static inline void writel(u32 val, volatile void *addr) { *(volatile u32 *)addr = val; }
static inline void writeq(u64 val, volatile void *addr) { *(volatile u64 *)addr = val; }

/* In a full implementation vmm_map_io should be used.
   Wait, to avoid circular dependencies in headers, we declare vmm_map_io if needed,
   or just declare it extern. */
extern void *vmm_map_io(phys_addr_t phys, size_t size);

/* ioremap / iounmap - map device MMIO into kernel virtual address space */
static inline void *ioremap(phys_addr_t phys_addr, size_t size) { return vmm_map_io(phys_addr, size); }
static inline void *ioremap_wc(phys_addr_t phys_addr, size_t size) { return vmm_map_io(phys_addr, size); }
static inline void *ioremap_nocache(phys_addr_t phys_addr, size_t size) { return vmm_map_io(phys_addr, size); }

static inline void iounmap(volatile void *addr) { (void)addr; }

/* I/O port string operations */
static inline void insb(u16 port, void *buf, int count) {
    u8 *p = (u8 *)buf;
    while (count--) {
        *p++ = inb(port);
    }
}

static inline void insw(u16 port, void *buf, int count) {
    u16 *p = (u16 *)buf;
    while (count--) {
        *p++ = inw(port);
    }
}

static inline void insl(u16 port, void *buf, int count) {
    u32 *p = (u32 *)buf;
    while (count--) {
        *p++ = inl(port);
    }
}

static inline void outsb(u16 port, const void *buf, int count) {
    const u8 *p = (const u8 *)buf;
    while (count--) {
        outb(port, *p++);
    }
}

static inline void outsw(u16 port, const void *buf, int count) {
    const u16 *p = (const u16 *)buf;
    while (count--) {
        outw(port, *p++);
    }
}

static inline void outsl(u16 port, const void *buf, int count) {
    const u32 *p = (const u32 *)buf;
    while (count--) {
        outl(port, *p++);
    }
}
