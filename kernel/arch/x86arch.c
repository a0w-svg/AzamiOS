#include "./include/x86arch.h"
#include "./include/gdt.h"
#include "./include/isr.h"
#include "../drivers/include/terminal.h"
#include "../klibc/include/stdio.h"
#include "../drivers/include/pit.h"
void x86_arch_init()
{
    gdt_init();
    terminal_clean();
    init_isr();
    printf("dffd %d < %s >\n", 234, "Ssdfdgkfgd vvv");
   __asm__ volatile("int $0xA");
   __asm__ volatile("sti");
   init_pit();
   set_pit_phase(50);
}