/**
 * kernel/mem/include/security.h — Stack Guard Canaries & ASLR Header for AzamiOS v6.0
 */
#ifndef SECURITY_H
#define SECURITY_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "../../klibc/include/kstack_guard.h"

/* Stack Canary Guard Init */
void stack_guard_init(void);

/* Address Space Layout Randomization (ASLR) */
void aslr_init(void);
uintptr_t aslr_randomize_base(uintptr_t base_addr, size_t alignment);
uintptr_t aslr_get_offset(void);

#endif /* SECURITY_H */
