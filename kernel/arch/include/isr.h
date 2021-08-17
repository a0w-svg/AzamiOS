#ifndef ISR_H
#define ISR_H

#include <stdint.h>

typedef struct 
{
    uint32_t ds;  // Data segment selector;
    uint32_t edi, esi, ebp, euseless, ebx, ecx, eax; // Pushed by pusha;
    uint32_t err_code, int_no; // Interrupt number and error code (if applicable);
    uint32_t eip, cs, eflags, esp, ss; // Pushed by the procesor automatically;
}registers_t;

#define IRQ0 32
#define IRQ1 33
#define IRQ2 34
#define IRQ3 35
#define IRQ4 36
#define IRQ5 37
#define IRQ6 38
#define IRQ7 39
#define IRQ8 40
#define IRQ9 41
#define IRQ10 42
#define IRQ11 43
#define IRQ12 44
#define IRQ13 45
#define IRQ14 46
#define IRQ15 47
/*
    Enables registration of callbacks for interrupts or IRQs; 
*/
typedef void (*isr_t)(registers_t*);
/*
    Initialize Iterrupt Service Routine;
*/
void init_isr();
/*
    Registers the interrupt handler;
*/
void register_interrupt_handler(uint8_t num, isr_t handler);

void exception_handler(registers_t *r);
void irq_handler(registers_t *r);
#endif