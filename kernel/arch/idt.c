#include <stdbool.h>
#include "./include/idt.h"
#include "../klibc/include/string.h"
#include "./include/pic.h"
#include "../klibc/include/port.h"

#define MAX_IDT_DESCRIPTORS 256 // Max number of IDT descriptors.

idt_entry_t idt[MAX_IDT_DESCRIPTORS]; // Create an array of IDT entries; aligned for performance
idt_register_t idtr;
static bool idt_sets[MAX_IDT_DESCRIPTORS];

void idt_set_gate(uint8_t num, uint32_t handler, uint16_t selector, uint8_t flags)
{
    idt_entry_t* idt_gate = &idt[num];
    idt_gate->base_low = (uint16_t)(handler & 0xFFFF); // Sets the lower 16 bits of the ISR address.
    idt_gate->kernel_cs = selector; // Sets offset to your kernel code selector.
    idt_gate->reserved = 0; // This variable always must be zero.
    idt_gate->attributes = flags;  // Sets the flags.
    idt_gate->base_high = (uint16_t)((handler >> 16) & 0xFFFF); // Sets the Higher 16 bits of the ISR address;
}

/*
    Initialize IDT;
*/
void idt_init()
{
    idtr.base = (uint32_t)&idt;
    idtr.limit = (sizeof(idt_entry_t) * MAX_IDT_DESCRIPTORS) - 1;
    
    __asm__ volatile ("lidtl (%0)" : : "r"(&idtr)); // load the new IDT
}
/*
    This function removes the specified idt entry from array of IDT entries;
*/
void idt_free_idt_gate(uint8_t gate)
{
    idt_set_gate(gate, 0, KERNEL_CODE_SEGMENT, 0);
    idt_sets[gate] = false;
}
/*
    This function allocates gate.
*/
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