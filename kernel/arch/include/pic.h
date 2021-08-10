#ifndef PIC_H
#define PIC_H
/*
                                AzamiOS PIC module
    The 8259 Programmable Interrupt Controller (PIC) is one of the most important chips
    making up the x86 architecture. Without it, the x86 architecture would not be an interrupt
    driven architecture. The function of the 8259A is to manage hardware interrupts and send them
    to the appropriate system interrupt. This allows the system to respond to devices needs without 
    loss of time (from polling the device, for instance;
*/
#include <stdint.h>
/*
    Initialize PIC;
*/
void init_PIC();
/*
    reinitialize the PIC controllers, giving them specified  vector offsets 
    rather than 8h and 70h, as configured by default;
    arguments:
        offset1 - vector offset for primary PIC;
        offset2 - vector offset for secondary PIC;
*/
void PIC_remap(uint8_t offset1, uint8_t offset2);
/*
    Send End-Of-Interrupt signal to irq device;
*/
void PIC_send_EOI(uint8_t irq);
/*
    Sets the ignore bit on the specifed IRQline;
*/
void IRQ_set_mask(uint8_t IRQline);
/*
    Clears the ignore bit on the specifed IRQline;
*/
void IRQ_clear_mask(uint8_t IRQline);
/*
    Returns the combined value of the cascaded PICs irq request register;
*/
uint16_t pic_get_irr();
/*
    Returns the combined value of the cascaded PICs in-service register;
*/
uint16_t pic_get_isr();
#endif