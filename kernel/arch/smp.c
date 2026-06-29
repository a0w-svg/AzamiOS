/**
 * smp.c  –  Symmetric Multiprocessing Bootstrapping
 */
#include "include/smp.h"
#include "include/apic.h"
#include "../mem/include/pmm.h"
#include "../klibc/include/string.h"
#include "../klibc/include/stdio.h"
#include "include/spinlock.h"

extern char smp_boot_start[];
extern char smp_boot_end[];
extern char gdt_table[];

struct gdt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

static volatile uint32_t g_ap_count = 1;

void ap_entry(void) {
    uint32_t id = apic_get_id();
    apic_init();

    atomic_inc(&g_ap_count);
    kprintf("smp: AP core #%d awakened and entering idle scheduler loop\n", id);

    for (;;) {
        asm volatile("sti; hlt");
    }
}

void smp_init(void) {
    kprintf("smp: detecting multi-core CPU topology...\n");
    apic_init();

#if defined(__x86_64__)
    uint32_t tramp_size = smp_boot_end - smp_boot_start;
    memcpy((void*)0x8000, smp_boot_start, tramp_size);

    struct {
        uint16_t limit;
        uint64_t base;
    } __attribute__((packed)) *gdt_p64 = (void*)0x8110;
    gdt_p64->limit = (8 * 8) - 1;
    gdt_p64->base = (uint64_t)(uintptr_t)gdt_table;

    uintptr_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    *((volatile uint64_t*)0x8108) = cr3;
    *((volatile uint64_t*)0x8120) = (uint64_t)(uintptr_t)ap_entry;

    for (uint8_t id = 1; id < 4; id++) {
        void *ap_stack = pmm_alloc_block();
        if (!ap_stack) break;
        *((volatile uint64_t*)0x8100) = ((uint64_t)(uintptr_t)ap_stack) + 4096;

        kprintf("smp: sending INIT-SIPI sequence to AP core #%d...\n", id);
        apic_send_init(id);
        apic_send_sipi(id, 0x08);

        int timeout = 0;
        uint32_t start_cnt = g_ap_count;
        while (g_ap_count == start_cnt && timeout++ < 500) {
            for (volatile int wait = 0; wait < 1000; wait++);
        }
    }
#else
    /* Copy real-mode trampoline code to physical 0x8000 */
    uint32_t tramp_size = smp_boot_end - smp_boot_start;
    memcpy((void*)0x8000, smp_boot_start, tramp_size);

    /* Write 32-bit temporary GDT pointer at 0x8040 */
    struct gdt_ptr *gdt_p = (struct gdt_ptr *)0x8040;
    gdt_p->limit = (8 * 8) - 1;
    gdt_p->base = (uint32_t)(uintptr_t)gdt_table;
    *((volatile uint32_t*)0x8054) = (uint32_t)(uintptr_t)ap_entry;

    /* Awaken AP cores #1, #2, #3 */
    for (uint8_t id = 1; id < 4; id++) {
        void *ap_stack = pmm_alloc_block();
        if (!ap_stack) break;
        *((volatile uint32_t*)0x8050) = ((uint32_t)(uintptr_t)ap_stack) + 4096;

        kprintf("smp: sending INIT-SIPI sequence to AP core #%d...\n", id);
        apic_send_init(id);
        apic_send_sipi(id, 0x08); /* Vector 0x08 = physical address 0x8000 */

        int timeout = 0;
        uint32_t start_cnt = g_ap_count;
        while (g_ap_count == start_cnt && timeout++ < 500) {
            for (volatile int wait = 0; wait < 1000; wait++);
        }
    }
#endif

    kprintf("smp: multi-core initialization complete (Total Active Cores: %d)\n", g_ap_count);
}
