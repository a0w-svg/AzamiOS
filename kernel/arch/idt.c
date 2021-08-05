#include <stdbool.h>
#include "./include/idt.h"
#include "../klibc/include/string.h"
#define KERNEL_CODE_SEGMENT 0x08 // The offset  your kernel code selector is in your GDT.
#define MAX_IDT_DESCRIPTORS 256 // Max number of IDT descriptors.

__attribute__((aligned(0x10)))
static idt_entry_t idt[MAX_IDT_DESCRIPTORS]; // Create an array of IDT entries; aligned for performance
static idt_register_t idtr;
static bool idt_sets[MAX_IDT_DESCRIPTORS];

void idt_set_gate(uint8_t num, void* interrupt_handler, uint16_t selector, uint8_t flags)
{
    idt_entry_t* idt_entry =  &idt[num];  // Creates pointer to the IDT entry.
    idt_entry->base_low = (uint32_t)interrupt_handler & 0xFFFF; // Sets the lower 16 bits of the ISR address.
    idt_entry->kernel_cs = selector; // Sets offset to your kernel code selector.
    idt_entry->attributes = flags;  // Sets the flags.
    idt_entry->base_high = (uint32_t)interrupt_handler >> 16; // Sets the Higher 16 bits of the ISR address;
    idt_entry->reserved = 0; // This variable always must be zero.
}

extern void* isr_stub_table[];
/*
*/
void idt_init()
{
    idtr.base = (uintptr_t)&idt;
    idtr.limit = (uint16_t)sizeof(idt_entry_t) * MAX_IDT_DESCRIPTORS - 1;
    memset(&idt, 0, sizeof(idt_entry_t)*256);
    for(uint8_t i = 0; i < 32; i++)
    {
        idt_set_gate(i, isr_stub_table[i], KERNEL_CODE_SEGMENT, 0x8E);
        idt_sets[i] = true;
    }
    __asm__ volatile ("lidt %0" : : "memory"(idtr)); // load the new IDT
    __asm__ volatile ("sti"); // set the interrupt flag
}
/*
    This function removes the specified idt entry from array of IDT entries;
*/
void idt_free_idt_gate(uint8_t gate)
{
    idt_set_gate(gate, 0, KERNEL_CODE_SEGMENT, 0);
    idt_sets[gate] = false;
}

uint8_t idt_allocate_gate()
{
    for(uint32_t i = 0; i < MAX_IDT_DESCRIPTORS; i++)
    {
        if(!idt_sets[i])
        {
            idt_sets[i] = true;
            return (uint8_t)i;
        }
    }
    return 0;
}