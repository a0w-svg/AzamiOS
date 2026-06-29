#ifndef GDT_H
#define GDT_H

#include <stdint.h>

void gdt_init(void);
void set_kernel_stack(uintptr_t stack_top);

#endif