/*
    AzamiOS GDT module
*/

#include <stdint.h>
#include "./include/gdt.h"
#include "../klibc/include/string.h"

#define LOW_GDT(value) (value & 0xFFFF)
#define HIGH_GDT(value) ((value >> 24) & 0xFF)
#define GRAN(value) ((value >> 16) & 0x0F)

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

gdt_entry_t gdt_table[5]; // GDT entries table.
gdt_ptr_t gdt_pointer;    // The pointer to the Global Descriptor Table.

/*
    This function sets the value of one GDT entry.
    A num variable determines the segment number.
    The base variable determines the segment base address.
    The limit variable determines the size of the segment.
    The access variable determines access flags.
    the granularity_segment variable determines the scaling of the segment limit field.  
*/

extern void gdt_flush(); // enables access to our asm function from our C code.

void set_gdt_gate(int32_t num, uint32_t base, uint32_t limit, uint8_t access, uint8_t granularity_segment)
{
    gdt_table[num].base_low = LOW_GDT(base);          // Sets the lower 16 bits of the GDT base;
    gdt_table[num].base_middle = (base >> 16) & 0xFF; // Sets the next  8 bits  of the GDT base;
    gdt_table[num].base_high = HIGH_GDT(base);        // Sets the last 8 bits of the GDT base;
    gdt_table[num].limit_low = LOW_GDT(limit);        // Sets the lower 16 bits of the GDT limit;
    gdt_table[num].granularity = (limit >> 16) & 0x0F;
    gdt_table[num].granularity |= granularity_segment & 0xF0; // Sets granularity;
    gdt_table[num].access = access; // sets access flags;
}

void gdt_init()
{
    gdt_pointer.limit = (sizeof(gdt_entry_t) * 5) - 1;
    gdt_pointer.base = &gdt_table[0];
    set_gdt_gate(0, 0, 0, 0, 0);                // Null segment;
    set_gdt_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xCF); // Code segment;
    set_gdt_gate(2, 0, 0xFFFFFFFF, 0x92, 0xCF); // Data segment;
    set_gdt_gate(3, 0, 0xFFFFFFFF, 0xFA, 0xCF); // User-mode code segment;
    set_gdt_gate(4, 0, 0xFFFFFFFF, 0xF2, 0xCF); // User-mode data segment;
  
    gdt_flush();
}