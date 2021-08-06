#ifndef PIC_H
#define PIC_H
/*
                                AzamiOS PIC module
    The 8259 Programmable Interrupt Controller (PIC) is one of the most important chips
    making up the x86 architecture. Without it, the x86 architecture would not be an interrupt
    driven architecture. The function of the 8259A is to manage hardware interrupts and send them
    to the appropriate system interrupt. This allows the system to respond to devices needs without 
    loss of time (from polling the device, for instance).
*/
#include <stdint.h>

void PIC_sendEOI(uint8_t irq);
/*
    Remaps the irq table.
    arguments:
        offset1 - vector offset for master PIC
            ve
*/
void PIC_remap(int offset1, int offset2);
#endif