#include "./include/isr.h"
#include "../drivers/include/terminal.h"
void exception_handler(registers_t *r)
{
    terminal_write("f");
    while(1);
}