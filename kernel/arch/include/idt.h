#ifndef IDT_H
#define IDT_H
#include <stdint.h>
#define BITS_32_INTERRUPT_GATE 0x8E
#define KERNEL_CODE_SEGMENT 0x08 // The offset  your kernel code selector is in your GDT.
typedef struct 
{
    uint16_t base_low;    // The lower 16 bits of the ISR's address;
    uint16_t kernel_cs;  // The GDT segment selector that the CPU will load into CS before calling the ISR;
    uint8_t reserved;    // Set always to zero;
    uint8_t attributes;  // Type and attributes;
    uint16_t base_high;   // The higher 16 bits of the ISR's address;
} __attribute__((packed)) idt_entry_t;

typedef struct
{
    uint16_t limit;
    uint32_t base;
} __attribute__((packed)) idt_register_t;

void idt_set_gate(uint8_t num, uint32_t handler , uint16_t selector, uint8_t flags);
void idt_init();
#endif