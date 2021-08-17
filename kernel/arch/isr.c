#include "./include/isr.h"
#include "../klibc/include/stdio.h"
#include "./include/pic.h"
#include "./include/idt.h"
#include "../klibc/include/port.h"
#include <stdint.h>

isr_t interrupt_handlers[256];

//interrupts code messages
char *exception_messages[] = {
    "Division By Zero",
    "Debug",
    "Non Maskable Interrupt",
    "Breakpoint",
    "Into Detected Overflow",
    "Out of Bounds",
    "Invalid Opcode",
    "No Coprocessor",

    "Double Fault",
    "Coprocessor Segment Overrun",
    "Bad TSS",
    "Segment Not Present",
    "Stack Fault",
    "General Protection Fault",
    "Page Fault",
    "Unknown Interrupt",

    "Coprocessor Fault",
    "Alignment Check",
    "Machine Check",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",

    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved"
};

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
    Initialize Iterrupt Service Routine;
*/
void init_isr()
{
    idt_set_gate(0, (uint32_t)isr_0, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(1, (uint32_t)isr_1, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(2, (uint32_t)isr_2, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(3, (uint32_t)isr_3, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(4, (uint32_t)isr_4, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(5, (uint32_t)isr_5, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(6, (uint32_t)isr_6, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(7, (uint32_t)isr_7, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(8, (uint32_t)isr_8, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(9, (uint32_t)isr_9, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(10, (uint32_t)isr_10, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(11, (uint32_t)isr_11, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(12, (uint32_t)isr_12, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(13, (uint32_t)isr_13, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(14, (uint32_t)isr_14, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(15, (uint32_t)isr_15, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(16, (uint32_t)isr_16, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(17, (uint32_t)isr_17, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(18, (uint32_t)isr_18, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(19, (uint32_t)isr_19, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(20, (uint32_t)isr_20, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(21, (uint32_t)isr_21, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(22, (uint32_t)isr_22, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(23, (uint32_t)isr_23, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(24, (uint32_t)isr_24, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(25, (uint32_t)isr_25, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(26, (uint32_t)isr_26, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(27, (uint32_t)isr_27, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(28, (uint32_t)isr_28, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(29, (uint32_t)isr_29, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(30, (uint32_t)isr_30, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(31, (uint32_t)isr_31, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    // irqs
    outb(0x20, 0x11);
    outb(0xA0, 0x11);
    outb(0x21, 0x20);
    outb(0xA1, 0x28);
    outb(0x21, 0x04);
    outb(0xA1, 0x02);
    outb(0x21, 0x01);
    outb(0xA1, 0x01);
    outb(0x21, 0x0);
    outb(0xA1, 0x0);
    idt_set_gate(32, (uint32_t)irq_0, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(33, (uint32_t)irq_1, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(34, (uint32_t)irq_2, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(35, (uint32_t)irq_3, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(36, (uint32_t)irq_4, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(37, (uint32_t)irq_5, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(38, (uint32_t)irq_6, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(39, (uint32_t)irq_7, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(40, (uint32_t)irq_8, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(41, (uint32_t)irq_9, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(42, (uint32_t)irq_10, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(43, (uint32_t)irq_11, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(44, (uint32_t)irq_12, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(45, (uint32_t)irq_13, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(46, (uint32_t)irq_14, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(47, (uint32_t)irq_15, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_init();
}

/*
    Interrupt handler; Gets called from asm interrupt handler stub;
*/
void exception_handler(registers_t *r)
{
    if(interrupt_handlers[r->int_no] != 0)
    {
        isr_t handler = interrupt_handlers[r->int_no];
        handler(r);
    }
    else
    {
        printf("received unhandled interrupt: %d %s", r->int_no, exception_messages[r->int_no]);
    }
}

/*
    Registers the interrupt handler.
*/
void register_interrupt_handler(uint8_t num, isr_t handler)
{
    interrupt_handlers[num] = handler;
}
/*
IRQ handler
*/
void irq_handler(registers_t *r)
{
    if(r->int_no >= 40)
        outb(0xA0, 0x20); // secondary mode
    // primary mode
    outb(0x20, 0x20);
    if(interrupt_handlers[r->int_no] != 0)
    {
        isr_t handler = interrupt_handlers[r->int_no];
        handler(r);
    }
}