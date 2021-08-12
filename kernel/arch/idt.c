#include <stdbool.h>
#include "./include/idt.h"
#include "../klibc/include/string.h"
#include "./include/pic.h"
#include "../klibc/include/port.h"
#define BITS_32_INTERRUPT_GATE 0x8E
#define KERNEL_CODE_SEGMENT 0x08 // The offset  your kernel code selector is in your GDT.
#define MAX_IDT_DESCRIPTORS 256 // Max number of IDT descriptors.

__attribute__((aligned(0x10)))
static idt_entry_t idt[MAX_IDT_DESCRIPTORS]; // Create an array of IDT entries; aligned for performance
static idt_register_t idtr;
static bool idt_sets[MAX_IDT_DESCRIPTORS];

void idt_set_gate(uint8_t num, void* interrupt_handler, uint16_t selector, uint8_t flags)
{
    idt[num].base_low = (uint32_t)interrupt_handler & 0xFFFF; // Sets the lower 16 bits of the ISR address.
    idt[num].kernel_cs = selector; // Sets offset to your kernel code selector.
    idt[num].attributes = flags;  // Sets the flags.
    idt[num].base_high = ((uint32_t)interrupt_handler >> 16) & 0xFFFF; // Sets the Higher 16 bits of the ISR address;
    idt[num].reserved = 0; // This variable always must be zero.
}

extern void isr_0();
extern void isr_1();
extern void isr_2();
extern void isr_3();
extern void isr_4();
extern void isr_5();
extern void isr_6();
extern void isr_7();
extern void isr_8();
extern void isr_9();
extern void isr_10();
extern void isr_11();
extern void isr_12();
extern void isr_13();
extern void isr_14();
extern void isr_15();
extern void isr_16();
extern void isr_17();
extern void isr_18();
extern void isr_19();
extern void isr_20();
extern void isr_21();
extern void isr_22();
extern void isr_23();
extern void isr_24();
extern void isr_25();
extern void isr_26();
extern void isr_27();
extern void isr_28();
extern void isr_29();
extern void isr_30();
extern void isr_31();
extern void irq_0();
extern void irq_1();
extern void irq_2();
extern void irq_3();
extern void irq_4();
extern void irq_5();
extern void irq_6();
extern void irq_7();
extern void irq_8();
extern void irq_9();
extern void irq_10();
extern void irq_11();
extern void irq_12();
extern void irq_13();
extern void irq_14();
extern void irq_15();
/*
    Initialize IDT;
*/
void idt_init()
{
    idtr.base = (uintptr_t)&idt;
    idtr.limit = (uint16_t)sizeof(idt_entry_t) * MAX_IDT_DESCRIPTORS - 1;
    memset(&idt, 0, sizeof(idt_entry_t)*256);
    // isrs
    idt_set_gate(0, isr_0, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(1, isr_1, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(2, isr_2, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(3, isr_3, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(4, isr_4, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(5, isr_5, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(6, isr_6, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(7, isr_7, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(8, isr_8, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(9, isr_9, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(10, isr_10, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(11, isr_11, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(12, isr_12, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(13, isr_13, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(14, isr_14, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(15, isr_15, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(16, isr_16, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(17, isr_17, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(18, isr_18, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(19, isr_19, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(20, isr_20, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(21, isr_21, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(22, isr_22, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(23, isr_23, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(24, isr_24, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(25, isr_25, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(26, isr_26, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(27, isr_27, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(28, isr_28, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(29, isr_29, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(30, isr_30, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(31, isr_31, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    // irqs
    init_PIC();
    idt_set_gate(32, irq_0, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(33, irq_1, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(34, irq_2, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(35, irq_3, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(36, irq_4, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(37, irq_5, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(38, irq_6, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(39, irq_7, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(40, irq_8, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(41, irq_9, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(42, irq_10, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(43, irq_11, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(44, irq_12, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(45, irq_13, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(46, irq_14, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(47, irq_15, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    __asm__ volatile ("lidtl %0" : : "memory"(idtr)); // load the new IDT
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