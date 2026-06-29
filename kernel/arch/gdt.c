/*
    AzamiOS GDT module
*/

#include <stdint.h>
#include <stdbool.h>
#include "./include/gdt.h"
#include "../klibc/include/string.h"

#define LOW_GDT(value) (uint16_t)(value & 0xFFFF)
#define HIGH_GDT(value) (uint16_t)((value >> 24) & 0xFF)
#define GRAN(value) (uint16_t)((value >> 16) & 0x0F)

typedef struct
{
    uint16_t limit_low;     // The lower 16 bits of the GDT limit;
    uint16_t base_low;      // The lower 16 bits of the GDT base;
    uint8_t base_middle;    // The next 8 bits of the GDT base;
    uint8_t access;         // Access flags, determine which the protection ring was used in this segment;
    uint8_t granularity;    // Granularity specifies the size of the segment;
    uint8_t base_high;      // The last 8 bits of the GDT base;
} __attribute__((packed)) gdt_entry_t;

typedef struct 
{
    uint16_t limit;     // The upper 16 bits of all GDT selector limits.
    gdt_entry_t *base;  // The address of the first gdt_entry_t struct.
} __attribute__((packed)) gdt_ptr_t;

gdt_entry_t gdt_table[8]; // GDT entries table.
gdt_ptr_t gdt_pointer;    // The pointer to the Global Descriptor Table.

typedef struct{
    uint32_t prev_tss;
    uint32_t esp0; // Kernel Stack Pointer
    uint32_t ss0; // Kernel segment Pointer
    uint32_t esp1, ss1, esp2, ss2;
    uint32_t cr3, eip, eflags, eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t es, cs, ss, ds, fs, gs;
    uint32_t ldt;
    uint32_t trap, iomap_base;
}__attribute__((packed)) tss_entry_t;

tss_entry_t tss_entry;

#if defined(__x86_64__)
struct tss64 {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;
} __attribute__((packed));

struct tss64 tss_entry64;
#endif

extern void gdt_flush(uintptr_t pointer);
extern bool g_is_uefi;

void set_gdt_gate(int32_t num, uintptr_t base, uint32_t limit, uint8_t access, uint8_t granularity_segment)
{
    gdt_entry_t* gdt_entry = &gdt_table[num];
    gdt_entry->base_low = LOW_GDT(base);
    gdt_entry->base_middle = (base >> 16) & 0xFF;
    gdt_entry->base_high = HIGH_GDT(base);
    gdt_entry->limit_low = LOW_GDT(limit);
    gdt_entry->granularity = (limit >> 16) & 0x0F;
    gdt_entry->granularity |= granularity_segment & 0xF0;
    gdt_entry->access = access;
}

void write_tss(int32_t num, uint16_t ss0, uintptr_t esp0){
    uintptr_t base = (uintptr_t)&tss_entry;
    uint32_t limit = sizeof(tss_entry_t) - 1;
    set_gdt_gate(num, base, limit, 0xE9, 0x00);
    memset(&tss_entry, 0, sizeof(tss_entry_t));
    tss_entry.ss0 = ss0;
    tss_entry.esp0 = (uint32_t)esp0;
    tss_entry.iomap_base = sizeof(tss_entry_t);
}

#if defined(__x86_64__)
void write_tss64(int32_t num, uintptr_t rsp0) {
    uintptr_t base = (uintptr_t)&tss_entry64;
    uint32_t limit = sizeof(struct tss64) - 1;
    memset(&tss_entry64, 0, sizeof(struct tss64));
    tss_entry64.rsp0 = rsp0;
    tss_entry64.iopb_offset = sizeof(struct tss64);
    set_gdt_gate(num, base & 0xFFFFFFFF, limit, 0x89, 0x00);
    uint64_t base_high = base >> 32;
    uint64_t *next_entry = (uint64_t*)&gdt_table[num + 1];
    *next_entry = base_high;
}
#endif

void set_kernel_stack(uintptr_t esp0) {
#if defined(__x86_64__)
    tss_entry64.rsp0 = esp0;
#else
    tss_entry.esp0 = (uint32_t)esp0;
#endif
}

uint8_t kernel_tss_stack[4096]__attribute__((aligned(4096)));
void gdt_init(void)
{
#if defined(__x86_64__)
    gdt_pointer.limit = (sizeof(gdt_entry_t) * 7) - 1;
    gdt_pointer.base = &gdt_table[0];
    set_gdt_gate(0, 0, 0, 0, 0);                // Null segment;
    set_gdt_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xAF); // 64-bit Kernel Code (L=1, D=0);
    set_gdt_gate(2, 0, 0xFFFFFFFF, 0x92, 0xAF); // 64-bit Kernel Data;
    set_gdt_gate(3, 0, 0xFFFFFFFF, 0xFA, 0xCF); // 32-bit Compatibility User Code (L=0, D=1);
    set_gdt_gate(4, 0, 0xFFFFFFFF, 0xF2, 0xCF); // User Data;
    write_tss64(5, (uintptr_t)kernel_tss_stack + 4096);
    gdt_flush((uintptr_t)&gdt_pointer);
    asm volatile("ltr %%ax" : : "a" (0x28));
#else
    gdt_pointer.limit = (sizeof(gdt_entry_t) * 6) - 1;
    gdt_pointer.base = &gdt_table[0];
    set_gdt_gate(0, 0, 0, 0, 0);                // Null segment;
    set_gdt_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xCF); // Code segment;
    set_gdt_gate(2, 0, 0xFFFFFFFF, 0x92, 0xCF); // Data segment;
    set_gdt_gate(3, 0, 0xFFFFFFFF, 0xFA, 0xCF); // User-mode code segment;
    set_gdt_gate(4, 0, 0xFFFFFFFF, 0xF2, 0xCF); // User-mode data segment;
    write_tss(5,0x10, (uintptr_t)kernel_tss_stack + 4096); //0x10 to selector Kernel Data Segment (2 * 8 = 16)
    gdt_flush((uintptr_t)&gdt_pointer);
    asm volatile("ltr %%ax" : : "a" (0x28));
#endif
}
