/*
    AzamiOS GDT module
*/

#include <stdint.h>
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

gdt_entry_t gdt_table[6]; // GDT entries table.
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



/*
    This function sets the value of one GDT entry.
    A num variable determines the segment number.
    The base variable determines the segment base address.
    The limit variable determines the size of the segment.
    The access variable determines access flags.
    the granularity_segment variable determines the scaling of the segment limit field.  
*/

extern void gdt_flush(uint32_t pointer); // enables access to our asm function from our C code.

void set_gdt_gate(int32_t num, uint32_t base, uint32_t limit, uint8_t access, uint8_t granularity_segment)
{
    gdt_entry_t* gdt_entry = &gdt_table[num];
    gdt_entry->base_low = LOW_GDT(base);          // Sets the lower 16 bits of the GDT base;
    gdt_entry->base_middle = (base >> 16) & 0xFF; // Sets the next  8 bits  of the GDT base;
    gdt_entry->base_high = HIGH_GDT(base);        // Sets the last 8 bits of the GDT base;
    gdt_entry->limit_low = LOW_GDT(limit);        // Sets the lower 16 bits of the GDT limit;
    gdt_entry->granularity = (limit >> 16) & 0x0F;
    gdt_entry->granularity |= granularity_segment & 0xF0; // Sets granularity;
    gdt_entry->access = access; // sets access flags;
}

void write_tss(int32_t num, uint16_t ss0, uint32_t esp0){
    uint32_t base = (uint32_t)&tss_entry;
    uint32_t limit = sizeof(tss_entry_t) - 1;
    
    // access flag 0xE9 means 32 bit available TSS for mode Ring 3
    set_gdt_gate(num, base, limit, 0xE9, 0x00);
    // zeroing structure 
    memset(&tss_entry, 0, sizeof(tss_entry_t));

    tss_entry.ss0 = ss0;
    tss_entry.esp0 = esp0;

    // set IOPB flag to block unauthorized access for user
    tss_entry.iomap_base = sizeof(tss_entry_t);
}

void set_kernel_stack(uint32_t esp0) {
    tss_entry.esp0 = esp0;
}

uint8_t kernel_tss_stack[4096]__attribute__((aligned(4096)));
void gdt_init()
{
    gdt_pointer.limit = (sizeof(gdt_entry_t) * 6) - 1;
    gdt_pointer.base = &gdt_table[0];
    set_gdt_gate(0, 0, 0, 0, 0);                // Null segment;
    set_gdt_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xCF); // Code segment;
    set_gdt_gate(2, 0, 0xFFFFFFFF, 0x92, 0xCF); // Data segment;
    set_gdt_gate(3, 0, 0xFFFFFFFF, 0xFA, 0xCF); // User-mode code segment;
    set_gdt_gate(4, 0, 0xFFFFFFFF, 0xF2, 0xCF); // User-mode data segment;
    // save current kernel esp
    uint32_t kernel_stack_top;
    asm volatile("mov %%esp, %0" : "=r"(kernel_stack_top));
    write_tss(5,0x10, (uint32_t)kernel_tss_stack + 4096); //0x10 to selector Kernel Data Segment (2 * 8 = 16)
    gdt_flush((uint32_t)&gdt_pointer);
    asm volatile("ltr %%ax" : : "a" (0x28));

}
