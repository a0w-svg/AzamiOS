#ifndef GDT_H
#define GDT_H

#include <stdint.h>

void gdt_init();
void set_kernel_stack(uint32_t esp0);
#endif