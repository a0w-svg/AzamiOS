/**
 * process.c  –  Process Control Block & Lifecycle Management
 */
#include "include/process.h"
#include "include/scheduler.h"
#include "../mem/include/pmm.h"
#include "../klibc/include/string.h"
#include "../klibc/include/stdio.h"

static uint32_t g_next_pid = 0;

void process_init(void) {
    g_next_pid = 0;
    kprintf("proc: process manager initialized\n");
}

process_t *process_create(const char *name, uintptr_t entry, uintptr_t cr3) {
    process_t *p = (process_t*)pmm_alloc_block();
    if (!p) return NULL;
    memset(p, 0, sizeof(process_t));

    p->pid = ++g_next_pid;
    strncpy(p->name, name, 31);
    p->state = PROC_READY;
    p->cr3 = cr3;

    void *phys_kstack = pmm_alloc_block();
    if (!phys_kstack) return NULL;
    p->kernel_stack = (uintptr_t)phys_kstack;

    /* Mock initial hardware stack frame for iret */
    uintptr_t *stack = (uintptr_t*)(p->kernel_stack + 4096);

    *(--stack) = 0x23;       /* SS (User Data Selector 0x20 | RPL 3) */
    *(--stack) = 0xC0000000; /* ESP (User Stack Top) */
    *(--stack) = 0x202;      /* EFLAGS (IF=1, Reserved Bit=1) */
    *(--stack) = 0x1B;       /* CS (User Code Selector 0x18 | RPL 3) */
    *(--stack) = entry;      /* EIP */

    *(--stack) = 0;          /* err_code */
    *(--stack) = 32;         /* int_no (IRQ0 timer) */

    /* pusha: eax, ecx, edx, ebx, euseless, ebp, esi, edi */
    for (int i = 0; i < 8; i++) *(--stack) = 0;

    *(--stack) = 0x23;       /* DS (User Data Selector) */

    p->kernel_esp = (uintptr_t)stack;
    scheduler_add(p);

    kprintf("proc: created process '%s' (PID %d, cr3=0x%x)\n", name, p->pid, (uint32_t)cr3);
    return p;
}

void process_exit(int exit_code) {
    process_t *cur = scheduler_get_current();
    if (cur) {
        cur->state = PROC_DEAD;
        kprintf("proc: process '%s' (PID %d) exited with code %d\n", cur->name, cur->pid, exit_code);
    }
    for (;;) asm volatile("hlt");
}
