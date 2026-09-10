/* ============================================================================
 * AzamiOS — Symmetric Multiprocessing (SMP) Implementation
 * File: arch/x86_64/cpu/smp.c
 * ============================================================================ */

#include "smp.h"
#include "cpu.h"
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

    /* Enable FPU, SSE, PGE, UMIP, FSGSBASE, OSXSAVE/AVX, SMEP, SMAP, NXE on AP */
    cpu_enable_features_ap();

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
    u64 new_rsp = g_cpu_infos[cpu_id].kernel_rsp0 - 16; /* BUG-F fix: must be 16-byte aligned before call */

    /* BUG-16 fix: pin new_rsp to RCX (caller-saved, not an argument register)
     * so the compiler cannot alias it with RDI.  The original `"r"(new_rsp)`
     * constraint allowed the compiler to choose RDI for new_rsp; if it did,
     * the `mov %%rcx, %%rsp` would have clobbered info (the first argument to
     * ap_c_entry) before the call could consume it.  With explicit RCX/RDI
     * constraints the register assignment is unambiguous and compiler-agnostic. */
    register u64 _rsp  __asm__("rcx") = new_rsp;
    register struct limine_smp_info *_info __asm__("rdi") = info;

    __asm__ volatile(
        "mov %%rcx, %%rsp \n\t"
        "call ap_c_entry  \n\t"
        :
        : "r"(_rsp), "r"(_info)
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
    /* BUG-E fix: iterate up to g_cpu_count (already capped) rather than the raw
     * Limine count, which may exceed SMP_MAX_CPUS and waste iterations. */
    for (u32 i = 0; i < smp_resp->cpu_count && ap_id < g_cpu_count; i++) {
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

    /* BUG-A fix: capture the atomic load once so the comparison and the log
     * message use the same consistent value (no torn read in kprintf). */
    u32 online = __atomic_load_n(&g_aps_online, __ATOMIC_SEQ_CST);
    if (online < expected_aps) {
        kprintf("[SMP] WARNING: Only %u/%u APs came online!\n",
                online, expected_aps);
    } else {
        kprintf("[SMP] All %u AP(s) successfully booted and online.\n", online);
    }
}

void smp_send_ipi(u32 cpu_id, u8 vector)
{
    if (cpu_id >= g_cpu_count || cpu_id == smp_current_cpu_id()) return;
    lapic_send_ipi(g_cpu_infos[cpu_id].lapic_id, vector);
}

void smp_send_reschedule(u32 cpu_id)
{
    smp_send_ipi(cpu_id, 49); /* Reschedule IPI vector 49 */
}
