#include "include/syscall.h"
#include "../arch/include/isr.h"
#include "../klibc/include/stdio.h"

// kernel function declaration that will be shared to user
static void syscall_print_string(char *str){
    kprintf("%s", str);
}

static void syscall_print_char(char c){
    putchar(c);
}
// main interrupt handler
void syscall_handler(registers_t *r){
    // number of system function in register EAX
    uint32_t syscall_number = r->eax;

    switch(syscall_number){
        case 0: // write string on the screen
            syscall_print_string((char*)r->ebx);
            break;

        case 1: // wirte single character on the screen
            syscall_print_char((char)r->ebx);
            break;
        default:
            kprintf("Unknown syscall: %d\n", syscall_number);
    }
}

// initialize handler
void init_syscalls(){
    register_interrupt_handler(128, syscall_handler);
}