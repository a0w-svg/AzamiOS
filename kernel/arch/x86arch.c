#include "./include/x86arch.h"
#include "./include/gdt.h"
#include "./include/idt.h"
#include "../drivers/include/terminal.h"
#include "../klibc/include/stdio.h"
void x86_arch_init()
{
    gdt_init();
    idt_init();
    terminal_clean();
    printf("dffd %d < %s >\n", 234, "Ssdfdgkfgd vvv");
    asm volatile("INT $0x3");
}