/**
 * kstack_guard.c – Kernel Stack Protector definitions
 */
#include "include/kstack_guard.h"
#include "include/kpanic.h"

uintptr_t __stack_chk_guard = STACK_CHK_GUARD;

__attribute__((noreturn)) void __stack_chk_fail(void) {
    PANIC("Stack smashing detected");
    for (;;) {
        asm volatile("cli; hlt");
    }
}
