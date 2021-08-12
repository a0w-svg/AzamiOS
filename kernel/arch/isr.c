#include "./include/isr.h"
#include "../klibc/include/stdio.h"
#include "./include/pic.h"

isr_t interrupt_handlers[256];
const char* exception_messages[] =
{
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
        printf("unhandled interrupt: %d \n", r->int_no);
        while(1);
    }
}

/*
    Registers the interrupt handler.
*/
void register_interrupt_handler(uint8_t num, isr_t handler)
{
    interrupt_handlers[num] = handler;
}

void irq_handler(registers_t *r)
{

}