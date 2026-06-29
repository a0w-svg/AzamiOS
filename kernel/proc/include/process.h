#ifndef PROCESS_H
#define PROCESS_H

#include <stdint.h>
#include <stdbool.h>
#include "../../arch/include/isr.h"

#define PROC_READY   0
#define PROC_RUNNING 1
#define PROC_BLOCKED 2
#define PROC_DEAD    3

typedef struct process {
    uint32_t pid;
    char name[32];
    uint32_t state;
    uintptr_t cr3;            // Physical page directory address
    uintptr_t kernel_esp;     // Saved kernel stack pointer (points to registers_t)
    uintptr_t kernel_stack;   // Base of allocated ring 0 stack (4KB)
    uintptr_t user_stack;     // User ring 3 stack
    uintptr_t entry;          // Entry point
    struct process *next;
} process_t;

void process_init(void);
process_t *process_create(const char *name, uintptr_t entry, uintptr_t cr3);
void process_exit(int exit_code);

#endif
