/* ============================================================================
 * AzamiOS — Local APIC Driver Implementation
 * File: arch/x86_64/cpu/lapic.c
 * ============================================================================ */

#include "lapic.h"
#include "msr.h"
#include "pic.h"
#include "idt.h"
#include "../mm/vmm.h"
#include "../../../drivers/char/console.h"
#include "../../../include/azami/defs.h"

/* LAPIC virtual address via HHDM */
static volatile u32 *g_lapic_mmio = NULL;
static u32 g_lapic_ticks_per_ms = 10000; /* Fallback until calibrated */

static inline void lapic_write(u32 reg, u32 val)
{
    g_lapic_mmio[reg / 4] = val;
}

static inline u32 lapic_read(u32 reg)
{
    return g_lapic_mmio[reg / 4];
}

phys_addr_t lapic_base_phys(void)
{
    u64 base = rdmsr(MSR_APIC_BASE);
    return (phys_addr_t)(base & 0xFFFFFFFFFFFFF000ULL);
}

void lapic_init(void)
{
    /* Enable APIC in MSR_APIC_BASE */
    u64 base = rdmsr(MSR_APIC_BASE);
    base |= (1ULL << 11); /* APIC Global Enable bit */
    wrmsr(MSR_APIC_BASE, base);

    phys_addr_t phys = lapic_base_phys();
    g_lapic_mmio = (volatile u32 *)PHYS_TO_VIRT(phys);

    /* Map LAPIC page in kernel page table with MMIO flags */
    vmm_map(vmm_kernel_space(), (virt_addr_t)g_lapic_mmio, phys, VMM_MMIO);

    /* Enable APIC via SVR and map spurious vector to 255 */
    lapic_write(LAPIC_SVR, LAPIC_SVR_ENABLE | LAPIC_SVR_SPURIOUS);

    /* Clear TPR so all priority interrupts can be received */
    lapic_write(LAPIC_TPR, 0);

    /* Acknowledge any outstanding interrupts */
    lapic_write(LAPIC_EOI, 0);

    kprintf("[LAPIC] Initialised at MMIO base 0x%016llx (ID=%u)\n",
            (unsigned long long)phys, lapic_id());
}

void lapic_eoi(void)
{
    if (g_lapic_mmio) {
        lapic_write(LAPIC_EOI, 0);
    }
}

u32 lapic_id(void)
{
    if (!g_lapic_mmio) return 0;
    return (lapic_read(LAPIC_ID) >> 24) & 0xFF;
}

void lapic_timer_start(u32 hz)
{
    if (!g_lapic_mmio || hz == 0) return;
    u32 count = (g_lapic_ticks_per_ms * 1000) / hz;
    if (count == 0) count = 100;

    lapic_write(LAPIC_TIMER_DIV, LAPIC_TIMER_DIV_16);
    lapic_write(LAPIC_LVT_TIMER, LAPIC_TIMER_PERIODIC | LAPIC_TIMER_VECTOR);
    lapic_write(LAPIC_TIMER_INIT, count);
}

void lapic_timer_stop(void)
{
    if (!g_lapic_mmio) return;
    lapic_write(LAPIC_TIMER_INIT, 0);
    lapic_write(LAPIC_LVT_TIMER, (1U << 16)); /* Mask timer */
}

void lapic_send_ipi(u32 lapic_id, u8 vector)
{
    if (!g_lapic_mmio) return;
    while (lapic_read(LAPIC_ICR_LO) & (1U << 12)) cpu_pause();
    lapic_write(LAPIC_ICR_HI, (lapic_id << 24));
    lapic_write(LAPIC_ICR_LO, LAPIC_ICR_FIXED | vector | LAPIC_ICR_ASSERT);
}

void lapic_send_init(u32 lapic_id)
{
    if (!g_lapic_mmio) return;
    while (lapic_read(LAPIC_ICR_LO) & (1U << 12)) cpu_pause();
    lapic_write(LAPIC_ICR_HI, (lapic_id << 24));
    lapic_write(LAPIC_ICR_LO, LAPIC_ICR_INIT | LAPIC_ICR_LEVEL | LAPIC_ICR_ASSERT);
}

void lapic_send_sipi(u32 lapic_id, u8 trampoline_page)
{
    if (!g_lapic_mmio) return;
    while (lapic_read(LAPIC_ICR_LO) & (1U << 12)) cpu_pause();
    lapic_write(LAPIC_ICR_HI, (lapic_id << 24));
    lapic_write(LAPIC_ICR_LO, LAPIC_ICR_SIPI | trampoline_page | LAPIC_ICR_ASSERT);
}
