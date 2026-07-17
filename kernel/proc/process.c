/**
 * process.c  –  Process Control Block & Lifecycle Management
 */
#include "include/process.h"
#include "include/scheduler.h"
#include "../mem/include/pmm.h"
#include "../mem/include/paging.h"
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
    p->on_cpu = false;
    p->cpu_id = 0;

    if (!cr3) {
        uintptr_t cloned = paging_clone_directory();
        if (cloned) cr3 = cloned;
    }
    p->cr3 = cr3;
    p->ring = 3; /* All processes launched from userspace run at ring 3 */

    void *phys_kstack = pmm_alloc_block();
    if (!phys_kstack) return NULL;
    p->kernel_stack = (uintptr_t)phys_kstack;

    /* Mock initial hardware stack frame for iret */
    uintptr_t *stack = (uintptr_t*)(p->kernel_stack + 4096);

    *(--stack) = 0x23;       /* SS (User Data Selector 0x20 | RPL 3) */
    *(--stack) = 0xC0000000; /* ESP (User Stack Top) */
    *(--stack) = 0x202;      /* EFLAGS (IF=1, Reserved Bit=1) */
    *(--stack) = 0x2B;       /* CS (64-bit User Code Selector 0x28 | RPL 3) */
    *(--stack) = entry;      /* EIP/RIP */

    *(--stack) = 0;          /* err_code */
    *(--stack) = 32;         /* int_no (IRQ0 timer) */

    /* pusha: eax, ecx, edx, ebx, euseless, ebp, esi, edi */
    for (int i = 0; i < 8; i++) *(--stack) = 0;

    *(--stack) = 0x23;       /* DS (User Data Selector) */

    p->kernel_esp = (uintptr_t)stack;
    scheduler_add(p);

    kprintf("proc: created process '%s' (PID %d, cr3=0x%llx)\n", name, p->pid, (unsigned long long)cr3);
    return p;
}

void process_exit(int exit_code) {
    process_t *cur = scheduler_get_current();
    if (cur) {
        cur->state = PROC_DEAD;
        kprintf("proc: process '%s' (PID %d) exited with code %d\n", cur->name, cur->pid, exit_code);
        if (cur->pid > 1) {
            scheduler_schedule();
            return;
        }
    }
    for (;;) asm volatile("hlt");
}

int process_thread_create(uintptr_t entry, uintptr_t arg, uintptr_t user_stack) {
    process_t *cur = scheduler_get_current();
    if (!cur) return -1;

    process_t *p = (process_t*)pmm_alloc_block();
    if (!p) return -1;
    memset(p, 0, sizeof(process_t));

    p->pid = ++g_next_pid;
    strncpy(p->name, cur->name, 31);
    p->state = PROC_READY;
    p->on_cpu = false;
    p->cpu_id = 0;
    p->cr3 = cur->cr3; /* Share parent address space */
    p->ring = cur->ring; /* Inherit privilege level from parent */


    void *phys_kstack = pmm_alloc_block();
    if (!phys_kstack) return -1;
    p->kernel_stack = (uintptr_t)phys_kstack;

    uintptr_t *stack = (uintptr_t*)(p->kernel_stack + 4096);
    *(--stack) = 0x23;       /* SS */
    *(--stack) = user_stack; /* ESP */
    *(--stack) = 0x202;      /* EFLAGS */
    *(--stack) = 0x2B;       /* CS (64-bit User Code Selector 0x28 | RPL 3) */
    *(--stack) = entry;      /* EIP/RIP */

    *(--stack) = 0;          /* err_code */
    *(--stack) = 32;         /* int_no */

    /* pusha: eax=arg, ecx, edx, ebx, euseless, ebp, esi, edi */
    *(--stack) = arg;        /* eax */
    for (int i = 0; i < 7; i++) *(--stack) = 0;

    *(--stack) = 0x23;       /* DS */

    p->kernel_esp = (uintptr_t)stack;
    scheduler_add(p);
    return p->pid;
}

int process_fork(registers_t *regs) {
    process_t *cur = scheduler_get_current();
    if (!cur || !regs) return -1;

    process_t *p = (process_t*)pmm_alloc_block();
    if (!p) return -1;
    memset(p, 0, sizeof(process_t));

    p->pid = ++g_next_pid;
    strncpy(p->name, cur->name, 31);
    p->state = PROC_READY;
    p->on_cpu = false;
    p->cpu_id = 0;
    uintptr_t cloned_cr3 = paging_clone_directory();

    p->cr3 = cloned_cr3 ? cloned_cr3 : cur->cr3;
    p->ring = cur->ring; /* Fork inherits parent privilege level */

    void *phys_kstack = pmm_alloc_block();
    if (!phys_kstack) return -1;
    p->kernel_stack = (uintptr_t)phys_kstack;

    uintptr_t *stack = (uintptr_t*)(p->kernel_stack + 4096);
    *(--stack) = regs->ss;
    *(--stack) = regs->esp;
    *(--stack) = regs->eflags;
    *(--stack) = regs->cs;
    *(--stack) = regs->eip;

    *(--stack) = 0;
    *(--stack) = 32;

    *(--stack) = 0; /* Child returns 0 in EAX */
    *(--stack) = regs->ecx;
    *(--stack) = regs->edx;
    *(--stack) = regs->ebx;
    *(--stack) = regs->esp;
    *(--stack) = regs->ebp;
    *(--stack) = regs->esi;
    *(--stack) = regs->edi;

    *(--stack) = regs->ds;

    p->kernel_esp = (uintptr_t)stack;
    kprintf("proc: fork created pid=%u parent=%s eip=0x%llx esp=0x%llx cs=0x%x ss=0x%x ds=0x%x\n", p->pid, cur->name, (unsigned long long)regs->eip, (unsigned long long)regs->esp, (uint32_t)regs->cs, (uint32_t)regs->ss, (uint32_t)regs->ds);
    scheduler_add(p);
    return p->pid;
}
