#ifndef IDT_H
#define IDT_H
#include <stdint.h>
#define BITS_32_INTERRUPT_GATE 0x8E
#define BITS_32_INTERRUPT_GATE_USER 0xEE
#define KERNEL_CODE_SEGMENT 0x08 // The offset  your kernel code selector is in your GDT.
typedef struct 
{
    uint16_t base_low;
    uint16_t kernel_cs;
    uint8_t ist;
    uint8_t attributes;
    uint16_t base_mid;
    uint32_t base_high;
    uint32_t reserved;
} __attribute__((packed)) idt_entry_t;

typedef struct
{
    uint16_t limit;
    uintptr_t base;
} __attribute__((packed)) idt_register_t;

void idt_set_gate(uint8_t num, uintptr_t handler , uint16_t selector, uint8_t flags);
void idt_init(void);
void idt_load_current(void);
#endif