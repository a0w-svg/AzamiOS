#include "./include/isr.h"
#include "../drivers/include/terminal.h"
#include "./include/pic.h"

isr_t interrupt_handlers[256];
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