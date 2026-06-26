#include "./include/kpanic.h"
#include "./include/stdio.h"
#include <stddef.h>

void print_backtrace(uint32_t max_frames)
{
    uint32_t *ebp;
    asm volatile("mov %%ebp, %0" : "=r"(ebp));
    kprintf("Stack Backtrace:\n");
    for (uint32_t i = 0; i < max_frames && ebp != NULL; i++) {
        uint32_t eip = ebp[1];
        if (eip == 0) break;
        kprintf("  [#%d] EIP=0x%x\n", i, eip);
        ebp = (uint32_t*)ebp[0];
        if ((uint32_t)ebp < 0x1000 || ((uint32_t)ebp & 3)) break;
    }
}

void dump_registers(void)
{
    uint32_t eax, ebx, ecx, edx, esi, edi, ebp, esp;
    asm volatile("mov %%eax, %0" : "=r"(eax));
    asm volatile("mov %%ebx, %0" : "=r"(ebx));
    asm volatile("mov %%ecx, %0" : "=r"(ecx));
    asm volatile("mov %%edx, %0" : "=r"(edx));
    asm volatile("mov %%esi, %0" : "=r"(esi));
    asm volatile("mov %%edi, %0" : "=r"(edi));
    asm volatile("mov %%ebp, %0" : "=r"(ebp));
    asm volatile("mov %%esp, %0" : "=r"(esp));
    kprintf("CPU Register Dump:\n");
    kprintf("  EAX=0x%x  EBX=0x%x  ECX=0x%x  EDX=0x%x\n", eax, ebx, ecx, edx);
    kprintf("  ESI=0x%x  EDI=0x%x  EBP=0x%x  ESP=0x%x\n", esi, edi, ebp, esp);
}

void kpanic(const char* msg, const char* file, uint32_t line)
{
    asm volatile("cli");
    kprintf("\n====================================================\n");
    kprintf("                   [ KERNEL PANIC ]                 \n");
    kprintf("====================================================\n");
    kprintf("Reason   : %s\n", msg);
    kprintf("Location : %s:%d\n\n", file, line);
    dump_registers();
    kprintf("\n");
    print_backtrace(8);
    kprintf("\n*** System Halted ***\n");
    for (;;) {
        asm volatile("hlt");
    }
}

void kpanic_assert(const char* file, uint32_t line, const char* descript)
{
    asm volatile("cli");
    kprintf("\n====================================================\n");
    kprintf("                 [ ASSERTION FAILED ]               \n");
    kprintf("====================================================\n");
    kprintf("Condition : %s\n", descript);
    kprintf("Location  : %s:%d\n\n", file, line);
    dump_registers();
    kprintf("\n");
    print_backtrace(8);
    kprintf("\n*** System Halted ***\n");
    for (;;) {
        asm volatile("hlt");
    }
}