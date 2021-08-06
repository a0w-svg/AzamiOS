#include "./include/x86arch.h"
#include "./include/gdt.h"
#include "./include/idt.h"
#include "../drivers/include/terminal.h"

void x86_arch_init()
{
    gdt_init();
    idt_init();
    terminal_clean();
    terminal_write("\n\n\n\n\n");
    terminal_write("fgfghdhgjdfhdgfhfg\n\n\n\n");
    terminal_write("fdgdfhgfhgh\n\n\n\n\n\n\n\n");
    terminal_write("fdgfdgfddghggh\n\n\n\n");
    terminal_write("fgdgdghgfhgfdh\n\n\n");
    terminal_write("hello\n");
    terminal_write("gg");

}