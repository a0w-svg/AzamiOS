/* ============================================================================
 * AzamiOS — HAL IRQ Subsystem
 * File: hal/irq.h
 * ============================================================================ */
#pragma once
#include "../include/azami/types.h"

/* Legacy ISA IRQ lines plus room for the GSIs an IO APIC can carry. The
 * redirection table on a typical chipset holds 24 entries; 64 covers boards
 * with a second IO APIC without making the descriptor table expensive. */
#define HAL_NR_IRQS  64

/**
 * hal_irq_enable() — Enable a hardware IRQ.
 *
 * If the IO APIC is available, routes the legacy IRQ through it via MADT
 * overrides to the requested vector, on a CPU chosen by hal_irq_pick_cpu().
 * Otherwise, falls back to unmasking the legacy 8259 PIC.
 */
void hal_irq_enable(u8 irq, u8 vector);

/**
 * hal_irq_disable() — Disable a hardware IRQ.
 */
void hal_irq_disable(u8 irq);

/**
 * hal_irq_eoi() — Acknowledge an interrupt.
 *
 * To be called from the central ISR dispatcher for vectors 32+.
 * Safely routes the EOI to either the LAPIC or the legacy PIC.
 */
void hal_irq_eoi(u8 vector);

/**
 * hal_irq_set_affinity(irq, cpu) — Re-route an already-enabled IRQ to a
 * different CPU. Returns 0, or a negative errno if the IRQ is not routed
 * through an IO APIC, @cpu is not online, or the platform cannot address
 * that CPU from the IO APIC (see the APIC-ID note in irq.c).
 *
 * This is what /proc/irq/N/smp_affinity writes through.
 */
int hal_irq_set_affinity(u8 irq, u32 cpu);

/** hal_irq_get_affinity(irq) — CPU an IRQ is currently routed to, or
 *  (u32)-1 when the IRQ is not routed at all. */
u32 hal_irq_get_affinity(u8 irq);

/** hal_irq_is_routed(irq) — true once hal_irq_enable() has routed @irq. */
bool hal_irq_is_routed(u8 irq);

/** hal_irq_count(irq) — number of times @irq has fired, all CPUs. */
u64 hal_irq_count(u8 irq);

/** hal_irq_account(vector) — bump the per-IRQ and per-CPU counters. Called
 *  from the ISR dispatcher for device vectors. */
void hal_irq_account(u8 vector);

/**
 * hal_irq_rebalance() — Redistribute every routed IRQ across the CPUs that
 * are online now.
 *
 * Interrupts enabled during early boot are routed while the APs are still
 * coming up, so they all land on the BSP by default. Called once from
 * kernel_main() after SMP bring-up completes, this spreads them out — which
 * matters most for the high-rate sources (NIC, AHCI) whose handlers would
 * otherwise contend with the BSP's own scheduling work.
 */
void hal_irq_rebalance(void);
