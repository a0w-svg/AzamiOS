#ifndef ISR_H
#define ISR_H

#include <stdint.h>

typedef struct 
{
    uint32_t ds;  // Data segment selector;
    uint32_t edi, esi, ebp, esp, ebx, ecx, eax; // Pushed by pusha;
    uint32_t int_no, err_code; // Interrupt number and error code (if applicable);
    uint32_t eip, cs, eflags, useresp, ss; // Pushed by the procesor automatically;
}registers_t;

void exception_handler(registers_t *r);

#endif