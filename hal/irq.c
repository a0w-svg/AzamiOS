/* ============================================================================
 * AzamiOS — HAL IRQ Subsystem
 * File: hal/irq.c
 *
 * Device interrupts used to be routed unconditionally to lapic_id() — the
 * APIC ID of whichever CPU happened to call hal_irq_enable(). Every driver
 * probes from kernel_main() on the BSP, so in practice that meant *every*
 * device interrupt in the system was delivered to CPU 0, on every machine,
 * forever. The other cores never took a device interrupt at all.
 *
 * That is not merely unbalanced. A high-rate source — a gigabit NIC taking
 * an interrupt per packet, an AHCI port completing commands — can saturate
 * the core it is pinned to, and that core is also the one running the
 * timekeeping tick and the bulk of the scheduler's bookkeeping. Linux solves
 * this with per-IRQ affinity, settable from userspace through
 * /proc/irq/N/smp_affinity; this is the same mechanism, with a round-robin
 * initial assignment so the default is already spread out.
 * ============================================================================ */

#include "irq.h"
#include "../arch/x86_64/cpu/pic.h"
#include "../arch/x86_64/cpu/lapic.h"
#include "../arch/x86_64/cpu/smp.h"
#include "../arch/x86_64/cpu/spinlock.h"
#include "../drivers/acpi/ioapic.h"
#include "../drivers/char/console.h"

typedef struct {
    u8   vector;        /* IDT vector this line was routed to    */
    u32  cpu;           /* logical CPU it is delivered to        */
    bool routed;        /* hal_irq_enable() has programmed it    */
    bool masked;        /* hal_irq_disable() has masked it       */
    u64  count;         /* times it has fired                    */
} irq_desc_t;

static irq_desc_t g_irq_desc[HAL_NR_IRQS];
static spinlock_t g_irq_lock = SPINLOCK_INIT;

/* Round-robin cursor for the initial CPU assignment. */
static u32 g_irq_rr_cursor = 0;

/* Vector -> IRQ line, so the dispatcher's per-vector accounting can find the
 * descriptor without every caller having to pass the line number back. */
static u8 g_vector_to_irq[256];
static bool g_vector_mapped[256];

/*
 * An IO APIC redirection entry carries an 8-bit destination field, whatever
 * mode the local APICs are in — the wider destination only becomes available
 * with interrupt remapping (VT-d / AMD-Vi), which this kernel does not
 * program yet. A CPU whose APIC ID does not fit in 8 bits therefore cannot be
 * an interrupt destination, and asking for one has to fail rather than
 * silently truncate the ID and deliver the interrupt to an unrelated core.
 */
static bool irq_cpu_addressable(u32 cpu)
{
    if (!smp_cpu_online(cpu)) return false;
    return smp_cpu_apic_id(cpu) <= 0xFF;
}

/* Choose the next CPU in round-robin order among those that can actually be
 * addressed from the IO APIC. Falls back to the calling CPU, which is always
 * addressable if anything is. */
static u32 irq_pick_cpu(void)
{
    u32 ncpus = smp_cpu_count();
    if (ncpus == 0) ncpus = 1;
    if (ncpus > SMP_MAX_CPUS) ncpus = SMP_MAX_CPUS;

    for (u32 i = 0; i < ncpus; i++) {
        u32 cand = (g_irq_rr_cursor + i) % ncpus;
        if (irq_cpu_addressable(cand)) {
            g_irq_rr_cursor = (cand + 1) % ncpus;
            return cand;
        }
    }
    return smp_current_cpu_id();
}

static void irq_program(u8 irq, u8 vector, u32 cpu)
{
    ioapic_set_irq(irq, vector, smp_cpu_apic_id(cpu));
}

void hal_irq_enable(u8 irq, u8 vector)
{
    if (!ioapic_is_active()) {
        /* Fallback: unmask the legacy 8259 PIC. It has no destination field
         * at all — everything goes to the BSP by construction. */
        if (irq < HAL_NR_IRQS) {
            irqflags_t f = spinlock_lock_irqsave(&g_irq_lock);
            g_irq_desc[irq].vector = vector;
            g_irq_desc[irq].cpu    = 0;
            g_irq_desc[irq].routed = true;
            g_irq_desc[irq].masked = false;
            g_vector_to_irq[vector] = irq;
            g_vector_mapped[vector] = true;
            spinlock_unlock_irqrestore(&g_irq_lock, f);
        }
        pic_clear_mask(irq);
        return;
    }

    if (irq >= HAL_NR_IRQS) {
        /* Outside the descriptor table: route it to the calling CPU and let
         * it run unaccounted rather than dropping the request. */
        ioapic_set_irq(irq, vector, lapic_id());
        return;
    }

    irqflags_t f = spinlock_lock_irqsave(&g_irq_lock);
    u32 cpu = irq_pick_cpu();
    g_irq_desc[irq].vector = vector;
    g_irq_desc[irq].cpu    = cpu;
    g_irq_desc[irq].routed = true;
    g_irq_desc[irq].masked = false;
    g_vector_to_irq[vector] = irq;
    g_vector_mapped[vector] = true;
    irq_program(irq, vector, cpu);
    spinlock_unlock_irqrestore(&g_irq_lock, f);
}

void hal_irq_disable(u8 irq)
{
    if (irq < HAL_NR_IRQS) {
        irqflags_t f = spinlock_lock_irqsave(&g_irq_lock);
        g_irq_desc[irq].masked = true;
        spinlock_unlock_irqrestore(&g_irq_lock, f);
    }

    if (ioapic_is_active()) {
        ioapic_mask_irq(irq);
    } else {
        pic_set_mask(irq);
    }
}

void hal_irq_eoi(u8 vector)
{
    if (ioapic_is_active()) {
        /* APIC mode: LAPIC handles EOI for all hardware interrupts except spurious (255) */
        if (vector != 255) {
            lapic_eoi();
        }
    } else {
        /* PIC mode: Hardware IRQs start at vector 32 */
        if (vector >= 32 && vector <= 47) {
            pic_eoi(vector - 32);
        }
    }
}

void hal_irq_account(u8 vector)
{
    cpu_info_t *cpu = smp_get_cpu();
    if (cpu) cpu->irq_count++;

    if (!g_vector_mapped[vector]) return;
    u8 irq = g_vector_to_irq[vector];
    if (irq < HAL_NR_IRQS)
        __atomic_add_fetch(&g_irq_desc[irq].count, 1, __ATOMIC_RELAXED);
}

int hal_irq_set_affinity(u8 irq, u32 cpu)
{
    if (irq >= HAL_NR_IRQS) return -22;          /* -EINVAL */
    if (!ioapic_is_active()) return -95;         /* -EOPNOTSUPP: PIC has no dest */
    if (!irq_cpu_addressable(cpu)) return -22;

    irqflags_t f = spinlock_lock_irqsave(&g_irq_lock);
    if (!g_irq_desc[irq].routed) {
        spinlock_unlock_irqrestore(&g_irq_lock, f);
        return -2;                               /* -ENOENT: nothing routed */
    }
    g_irq_desc[irq].cpu = cpu;
    /* Re-programming writes the destination while the entry is masked and
     * unmasks it afterwards (see ioapic_set_irq), which is the sequence the
     * IO APIC requires: changing the destination of a live level-triggered
     * entry can otherwise leave the interrupt asserted at the old target. */
    if (!g_irq_desc[irq].masked)
        irq_program(irq, g_irq_desc[irq].vector, cpu);
    spinlock_unlock_irqrestore(&g_irq_lock, f);
    return 0;
}

u32 hal_irq_get_affinity(u8 irq)
{
    if (irq >= HAL_NR_IRQS) return (u32)-1;
    if (!g_irq_desc[irq].routed) return (u32)-1;
    return g_irq_desc[irq].cpu;
}

bool hal_irq_is_routed(u8 irq)
{
    return irq < HAL_NR_IRQS && g_irq_desc[irq].routed;
}

u64 hal_irq_count(u8 irq)
{
    if (irq >= HAL_NR_IRQS) return 0;
    return __atomic_load_n(&g_irq_desc[irq].count, __ATOMIC_RELAXED);
}

void hal_irq_rebalance(void)
{
    if (!ioapic_is_active()) return;

    u32 ncpus = smp_cpu_count();
    if (ncpus <= 1) return;

    irqflags_t f = spinlock_lock_irqsave(&g_irq_lock);
    g_irq_rr_cursor = 0;

    u32 moved = 0;
    for (u32 i = 0; i < HAL_NR_IRQS; i++) {
        if (!g_irq_desc[i].routed || g_irq_desc[i].masked) continue;

        /* IRQ 0 stays wherever it is. The legacy PIT is the one source whose
         * handler assumes it runs on the boot CPU, and on an APIC system it
         * is masked anyway — moving it buys nothing and risks a surprise. */
        if (i == 0) continue;

        u32 cpu = irq_pick_cpu();
        if (cpu == g_irq_desc[i].cpu) continue;
        g_irq_desc[i].cpu = cpu;
        irq_program((u8)i, g_irq_desc[i].vector, cpu);
        moved++;
    }
    spinlock_unlock_irqrestore(&g_irq_lock, f);

    if (moved)
        kprintf("[IRQ] Rebalanced %u interrupt line(s) across %u CPU(s)\n",
                moved, ncpus);
}
