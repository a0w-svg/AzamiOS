#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>
#include "../arch/include/isr.h"

uintptr_t syscall_handler(registers_t *r);
void init_syscalls(void);
#endif