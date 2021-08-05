#ifndef IDT_H
#define IDT_H
#include <stdint.h>

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

void idt_set_gate(uint8_t num, void* interrupt_handler, uint16_t selector, uint8_t flags);
void idt_init();
#endif