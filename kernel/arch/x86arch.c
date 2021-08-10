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
    printf("%s", "HEllo\n");
    printf("%d", 234);
    printf("%s", "\na\n");
    printf("%x", 0x1A);
}