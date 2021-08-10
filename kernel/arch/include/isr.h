#ifndef ISR_H
#define ISR_H

#include <stdint.h>

typedef struct 
{
    uint32_t ds;  // Data segment selector;
    uint32_t edi, esi, ebp, euseless, ebx, ecx, eax; // Pushed by pusha;
    uint32_t int_no, err_code; // Interrupt number and error code (if applicable);
    uint32_t eip, cs, eflags, esp, ss; // Pushed by the procesor automatically;
}registers_t;

/*
    Enables registration of callbacks for interrupts or IRQs; 
*/
typedef void (*isr_t)(registers_t*);

/*
    Registers the interrupt handler.
*/
void register_interrupt_handler(uint8_t num, isr_t handler);

#endif