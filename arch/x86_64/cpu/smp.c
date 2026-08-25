/* ============================================================================
 * AzamiOS — Symmetric Multiprocessing (SMP) Implementation
 * File: arch/x86_64/cpu/smp.c
 * ============================================================================ */

#include "smp.h"
#include "gdt.h"
#include "idt.h"
#include "lapic.h"
#include "msr.h"
#include "spinlock.h"
#include "../boot/limine_req.h"
#include "../mm/vmm.h"
#include "../../../kernel/mm/pmm.h"
#include "../../../drivers/char/console.h"
#include "../../../include/azami/defs.h"

static cpu_info_t g_cpu_infos[SMP_MAX_CPUS];
static u32 g_cpu_count = 1;
static volatile u32 g_aps_online = 0;

/* Per-CPU kernel stack size: 16 KB (4 pages) */
#define KERNEL_STACK_PAGES  4
#define KERNEL_STACK_SIZE   (KERNEL_STACK_PAGES * PAGE_SIZE)

extern volatile struct limine_smp_request g_limine_smp_req;

u32 smp_cpu_count(void)
{
    return g_cpu_count;
}

volatile bool g_smp_sched_active = false;

void ap_c_entry(struct limine_smp_info *info)
{
    if (!info) return;

    u32 cpu_id = (u32)info->extra_argument;
    cpu_info_t *cpu = &g_cpu_infos[cpu_id];

    /* GS_BASE = kernel cpu_info (active right now in ring 0). Set FIRST so smp_get_cpu() works. */
    wrmsr(MSR_GS_BASE, (u64)(uintptr_t)cpu);
    wrmsr(MSR_KERNEL_GS_BASE, 0ULL);

    /* Load kernel page table on AP */
    vmm_switch(vmm_kernel_space());

    /* Initialize GDT and TSS for this AP */
    gdt_init_ap(cpu_id, cpu->kernel_rsp0);

    /* Load IDT */
    idt_init();

    /* Initialize Local APIC on this CPU */
    lapic_init();

    /* Enable FPU & SSE/SSE2 on AP (clear EM/TS, set MP, OSFXSR, OSXMMEXCPT) */
    u64 cr0 = read_cr0();
    cr0 &= ~(1ULL << 2); /* clear EM */
    cr0 &= ~(1ULL << 3); /* clear TS */
    cr0 |=  (1ULL << 1); /* set MP */
    write_cr0(cr0);

    /* Apply OSFXSR / OSXMMEXCPT / SMEP / SMAP / FSGSBASE / OSXSAVE on AP */
    extern u8 g_smep_enabled;
    extern u8 g_smap_enabled;
    extern u8 g_fsgsbase_enabled;
    extern u8 g_osxsave_enabled;
    u64 cr4 = read_cr4();
    cr4 |= (1ULL << 9);  /* OSFXSR */
    cr4 |= (1ULL << 10); /* OSXMMEXCPT */
    if (g_fsgsbase_enabled) cr4 |= (1ULL << 16);
    if (g_osxsave_enabled)  cr4 |= (1ULL << 18);
    if (g_smep_enabled)     cr4 |= (1ULL << 20);
    if (g_smap_enabled)     cr4 |= (1ULL << 21);
    write_cr4(cr4);

    if (g_osxsave_enabled) {
        xsetbv(0, 0x7); /* x87 + SSE + AVX */
    }


    u64 efer = rdmsr(MSR_EFER);
    efer |= EFER_NXE;
    wrmsr(MSR_EFER, efer);

    /* Enable SYSCALL / SYSRET on AP */
    extern void syscall_abi_init(void);
    syscall_abi_init();

    /* Signal that this AP is online */
    __atomic_add_fetch(&g_aps_online, 1, __ATOMIC_SEQ_CST);

    /* Wait until BSP finishes early kernel initialization and activates scheduler */
    while (!__atomic_load_n(&g_smp_sched_active, __ATOMIC_SEQ_CST)) {
        cpu_pause();
    }

    /* Start Local APIC periodic preemption timer on this AP (100 Hz = 10ms tick) */
    lapic_timer_start(100);

    /* Start CFS scheduler on this AP */
    extern void sched_start(void);
    sched_start();
}

static void ap_entry(struct limine_smp_info *info)
{
    u32 cpu_id = (u32)info->extra_argument;
    u64 new_rsp = g_cpu_infos[cpu_id].kernel_rsp0 - 8;

    __asm__ volatile(
        "mov %0, %%rsp \n\t"
        "call ap_c_entry \n\t"
        :
        : "r"(new_rsp), "D"(info)
        : "memory"
    );
    __builtin_unreachable();
}

void smp_init(void)
{
    struct limine_smp_response *smp_resp = g_limine_smp_req.response;

    if (!smp_resp || smp_resp->cpu_count == 0) {
        kprintf("[SMP] Limine SMP response unavailable. Running in single-CPU (BSP only) mode.\n");
        g_cpu_count = 1;
    } else {
        g_cpu_count = (smp_resp->cpu_count > SMP_MAX_CPUS) ? SMP_MAX_CPUS : (u32)smp_resp->cpu_count;
        kprintf("[SMP] Limine reported %u total CPU(s). Initializing APs...\n", g_cpu_count);
    }

    /* Setup BSP cpu_info (CPU 0) */
    cpu_info_t *bsp = &g_cpu_infos[0];
    __builtin_memset(bsp, 0, sizeof(cpu_info_t));
    bsp->cpu_id = 0;
    bsp->lapic_id = (smp_resp && smp_resp->cpu_count > 0) ? smp_resp->bsp_lapic_id : 0;
    bsp->is_bsp = true;
    bsp->self = bsp;

    /* Allocate and assign kernel stack for BSP */
    phys_addr_t bsp_stack_phys = pmm_alloc_pages(KERNEL_STACK_PAGES);
    if (!bsp_stack_phys) PANIC("Failed to allocate BSP kernel stack!");
    bsp->kernel_rsp0 = (u64)PHYS_TO_VIRT(bsp_stack_phys) + KERNEL_STACK_SIZE;

    /* GS_BASE  = kernel cpu_info for BSP. Set FIRST before gdt_init_ap. */
    wrmsr(MSR_GS_BASE, (u64)(uintptr_t)bsp);
    wrmsr(MSR_KERNEL_GS_BASE, 0ULL);

    /* Reinitialize GDT/TSS for BSP with the new allocated stack */
    gdt_init_ap(0, bsp->kernel_rsp0);

    /* Initialize LAPIC on BSP */
    lapic_init();

    if (!smp_resp || g_cpu_count <= 1) {
        kprintf("[SMP] BSP online (ID=0, LAPIC_ID=%u)\n", bsp->lapic_id);
        return;
    }

    /* Boot all APs */
    u32 ap_id = 1; /* BSP always gets ID 0; APs get 1, 2, 3... */
    for (u32 i = 0; i < smp_resp->cpu_count && ap_id < SMP_MAX_CPUS; i++) {
        struct limine_smp_info *info = smp_resp->cpus[i];
        if (info->lapic_id == smp_resp->bsp_lapic_id) continue; /* Skip BSP */

        u32 id = ap_id++;
        cpu_info_t *cpu = &g_cpu_infos[id];
        __builtin_memset(cpu, 0, sizeof(cpu_info_t));
        cpu->cpu_id = id;
        cpu->lapic_id = info->lapic_id;
        cpu->is_bsp = false;
        cpu->self = cpu;

        phys_addr_t stack_phys = pmm_alloc_pages(KERNEL_STACK_PAGES);
        if (!stack_phys) PANIC("Failed to allocate AP kernel stack!");
        cpu->kernel_rsp0 = (u64)PHYS_TO_VIRT(stack_phys) + KERNEL_STACK_SIZE;

        info->extra_argument = (u64)id;
        __atomic_store_n(&info->goto_address, ap_entry, __ATOMIC_RELEASE);
    }

    /* Wait for all APs to come online (with timeout) */
    u32 expected_aps = g_cpu_count - 1;
    u64 timeout = 2000000ULL;
    while (__atomic_load_n(&g_aps_online, __ATOMIC_SEQ_CST) < expected_aps && timeout > 0) {
        cpu_pause();
        timeout--;
    }

    if (__atomic_load_n(&g_aps_online, __ATOMIC_SEQ_CST) < expected_aps) {
        kprintf("[SMP] WARNING: Only %u/%u APs came online!\n",
                g_aps_online, expected_aps);
    } else {
        kprintf("[SMP] All %u AP(s) successfully booted and online.\n", g_aps_online);
    }
}

void smp_send_reschedule(u32 cpu_id)
{
    if (cpu_id >= g_cpu_count || cpu_id == smp_current_cpu_id()) return;
    lapic_send_ipi(g_cpu_infos[cpu_id].lapic_id, 49); /* Reschedule IPI vector 49 */
}
