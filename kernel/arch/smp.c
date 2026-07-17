#include "include/smp.h"
#include "include/apic.h"
#include "include/idt.h"
#include "../mem/include/pmm.h"
#include "../klibc/include/string.h"
#include "../klibc/include/stdio.h"
#include "include/spinlock.h"
#include "../drivers/include/acpi.h"

extern char smp_boot_start[];
extern char smp_boot_end[];
extern char gdt_table[];
extern void init_syscalls(void);
extern struct process *scheduler_create_idle_task(uint32_t core_id);

cpu_data_t g_cpu_data[MAX_CPUS];
static volatile uint32_t g_ap_count = 1;
static volatile uint32_t g_ap_boot_stage = 0;

volatile uint32_t g_tlb_shootdown_active = 0;
volatile uintptr_t g_tlb_shootdown_addr = 0;
volatile uint32_t g_tlb_shootdown_ack_count = 0;
volatile uint32_t g_tlb_shootdown_target_count = 0;
volatile int g_tlb_shootdown_lock = 0;

cpu_data_t *smp_get_current_cpu(void) {
    uint32_t id = 0;
    if (g_apic_enabled) {
        id = apic_get_id();
    }
    if (id >= MAX_CPUS) id = 0;
    return &g_cpu_data[id];
}

uint32_t smp_get_cpu_count(void) {
    return __atomic_load_n(&g_ap_count, __ATOMIC_ACQUIRE);
}

bool smp_is_active(void) {
    return smp_get_cpu_count() > 1;
}

void smp_tlb_shootdown(uintptr_t virt_addr) {
    if (!smp_is_active()) return;
    unsigned long flags;
    spinlock_acquire_irqsave(&g_tlb_shootdown_lock, &flags);

    uint32_t num_targets = smp_get_cpu_count() - 1;
    if (num_targets > 0) {
        __atomic_store_n(&g_tlb_shootdown_addr, virt_addr, __ATOMIC_RELEASE);
        __atomic_store_n(&g_tlb_shootdown_ack_count, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&g_tlb_shootdown_target_count, num_targets, __ATOMIC_RELEASE);
        asm volatile("mfence" ::: "memory");
        __atomic_store_n(&g_tlb_shootdown_active, 1, __ATOMIC_RELEASE);

        apic_broadcast_ipi_exclude_self(252); /* Vector 252 = TLB_SHOOTDOWN_VECTOR */

        uint32_t timeout = 0;
        while (__atomic_load_n(&g_tlb_shootdown_ack_count, __ATOMIC_ACQUIRE) < num_targets && timeout < 5000000) {
            asm volatile("pause" ::: "memory");
            timeout++;
        }
        __atomic_store_n(&g_tlb_shootdown_active, 0, __ATOMIC_RELEASE);
    }

    spinlock_release_irqrestore(&g_tlb_shootdown_lock, flags);
}

void ap_entry(void) {
    idt_load_current();
    uint32_t id = apic_get_id();
    if (id >= MAX_CPUS) {
        for (;;) asm volatile("cli; hlt");
    }

    apic_init();

    cpu_data_t *cpu = &g_cpu_data[id];
    cpu->cpu_id = id;
    cpu->active = true;
    cpu->active_context = 0;
    cpu->current_proc = NULL;

    gdt_init_cpu(id, (uintptr_t)cpu->kernel_tss_stack + sizeof(cpu->kernel_tss_stack));

    init_syscalls();

    cpu->idle_proc = scheduler_create_idle_task(id);
    cpu->current_proc = cpu->idle_proc;

    __atomic_add_fetch(&g_ap_count, 1, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_ap_boot_stage, 1, __ATOMIC_RELEASE);
    kprintf("smp: AP core #%u awakened, synchronized, and entering idle loop\n", id);

    for (;;) {
        asm volatile("sti; hlt");
    }
}

void smp_init(void) {
    kprintf("smp: detecting multi-core CPU topology...\n");
    apic_init();

    uint32_t bsp_id = apic_get_id();
    if (bsp_id < MAX_CPUS) {
        g_cpu_data[bsp_id].cpu_id = bsp_id;
        g_cpu_data[bsp_id].active = true;
        g_cpu_data[bsp_id].active_context = 0;
        if (!g_cpu_data[bsp_id].idle_proc) {
            g_cpu_data[bsp_id].idle_proc = scheduler_create_idle_task(bsp_id);
        }
    }

    uint8_t apic_ids[MAX_CPUS];
    int num_cpus = acpi_get_cpus(apic_ids, MAX_CPUS);
    if (num_cpus <= 1) {
        kprintf("smp: ACPI MADT reported %d cores (or fallback required). Probing up to 4 cores...\n", num_cpus);
        num_cpus = 0;
        for (uint8_t i = 0; i < 4 && i < MAX_CPUS; i++) {
            apic_ids[num_cpus++] = i;
        }
    } else {
        kprintf("smp: ACPI MADT reported %d active CPU cores.\n", num_cpus);
    }

    uint32_t tramp_size = (uint32_t)(smp_boot_end - smp_boot_start);
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

    for (int i = 0; i < num_cpus; i++) {
        uint8_t id = apic_ids[i];
        if (id == bsp_id || id >= MAX_CPUS) continue;

        void *ap_stack = pmm_alloc_block();
        if (!ap_stack) continue;

        *((volatile uint64_t*)0x8100) = ((uint64_t)(uintptr_t)ap_stack) + 4096;

        __atomic_store_n(&g_ap_boot_stage, 0, __ATOMIC_RELEASE);

        kprintf("smp: sending INIT-SIPI sequence to AP core #%u...\n", id);
        apic_send_init(id);
        apic_send_sipi(id, 0x08); /* Vector 0x08 = physical address 0x8000 */

        uint32_t timeout = 0;
        while (__atomic_load_n(&g_ap_boot_stage, __ATOMIC_ACQUIRE) == 0 && timeout < 1000000) {
            asm volatile("pause" ::: "memory");
            timeout++;
            if (timeout == 50000) {
                apic_send_sipi(id, 0x08);
            }
        }

        if (__atomic_load_n(&g_ap_boot_stage, __ATOMIC_ACQUIRE) == 1) {
            kprintf("smp: AP core #%u successfully awakened and synchronized.\n", id);
        } else {
            kprintf("smp: WARNING: AP core #%u failed to synchronize (timeout).\n", id);
        }
    }

    kprintf("smp: multi-core initialization complete (Total Active Cores: %u)\n", smp_get_cpu_count());
}


