#include "./include/kpanic.h"
#include "./include/stdio.h"
#include "../proc/include/scheduler.h"
#include <stddef.h>

void print_backtrace(uint32_t max_frames)
{
    uintptr_t *rbp;
    asm volatile("mov %%rbp, %0" : "=r"(rbp));
    kprintf("Stack Backtrace:\n");
    for (uint32_t i = 0; i < max_frames && rbp != NULL; i++) {
        uintptr_t rip = rbp[1];
        if (rip == 0) break;
        kprintf("  [#%d] RIP=0x%016llx\n", i, (unsigned long long)rip);
        rbp = (uintptr_t*)rbp[0];
        if ((uintptr_t)rbp < 0x400000 || ((uintptr_t)rbp & 7)) break;
    }
}

void dump_registers(void)
{
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp, rsp;
    uint64_t cr0, cr2, cr3, cr4, rflags;
    asm volatile("mov %%rax, %0" : "=r"(rax));
    asm volatile("mov %%rbx, %0" : "=r"(rbx));
    asm volatile("mov %%rcx, %0" : "=r"(rcx));
    asm volatile("mov %%rdx, %0" : "=r"(rdx));
    asm volatile("mov %%rsi, %0" : "=r"(rsi));
    asm volatile("mov %%rdi, %0" : "=r"(rdi));
    asm volatile("mov %%rbp, %0" : "=r"(rbp));
    asm volatile("mov %%rsp, %0" : "=r"(rsp));
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    asm volatile("mov %%cr2, %0" : "=r"(cr2));
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    asm volatile("mov %%cr4, %0" : "=r"(cr4));
    asm volatile("pushfq; popq %0" : "=r"(rflags));
    kprintf("CPU Register Dump (64-bit):\n");
    kprintf("  RAX=0x%016llx  RBX=0x%016llx  RCX=0x%016llx  RDX=0x%016llx\n", (unsigned long long)rax, (unsigned long long)rbx, (unsigned long long)rcx, (unsigned long long)rdx);
    kprintf("  RSI=0x%016llx  RDI=0x%016llx  RBP=0x%016llx  RSP=0x%016llx\n", (unsigned long long)rsi, (unsigned long long)rdi, (unsigned long long)rbp, (unsigned long long)rsp);
    kprintf("  CR0=0x%016llx  CR2=0x%016llx  CR3=0x%016llx  CR4=0x%016llx\n", (unsigned long long)cr0, (unsigned long long)cr2, (unsigned long long)cr3, (unsigned long long)cr4);
    kprintf("  RFLAGS=0x%016llx\n", (unsigned long long)rflags);
}

void kpanic(const char* msg, const char* file, uint32_t line)
{
    asm volatile("cli");
    process_t *cur = scheduler_get_current();
    kprintf("\n====================================================\n");
    kprintf("                   [ KERNEL PANIC ]                 \n");
    kprintf("====================================================\n");
    kprintf("Process  : %s (PID %u)\n", cur ? cur->name : "none", cur ? cur->pid : 0);
    kprintf("Reason   : %s\n", msg);
    kprintf("Location : %s:%d\n\n", file, line);
    dump_registers();
    kprintf("\n");
    print_backtrace(16);
    kprintf("\n*** System Halted ***\n");
    for (;;) {
        asm volatile("hlt");
    }
}

void kpanic_assert(const char* file, uint32_t line, const char* descript)
{
    asm volatile("cli");
    process_t *cur = scheduler_get_current();
    kprintf("\n====================================================\n");
    kprintf("                 [ ASSERTION FAILED ]               \n");
    kprintf("====================================================\n");
    kprintf("Process   : %s (PID %u)\n", cur ? cur->name : "none", cur ? cur->pid : 0);
    kprintf("Condition : %s\n", descript);
    kprintf("Location  : %s:%d\n\n", file, line);
    dump_registers();
    kprintf("\n");
    print_backtrace(16);
    kprintf("\n*** System Halted ***\n");
    for (;;) {
        asm volatile("hlt");
    }
}