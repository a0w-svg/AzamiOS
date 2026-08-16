/* ============================================================================
 * AzamiOS — HAL IRQ Subsystem
 * File: hal/irq.h
 * ============================================================================ */
#pragma once
#include "../include/azami/types.h"

/**
 * hal_irq_enable() — Enable a hardware IRQ.
 * 
 * If the IO APIC is available, routes the legacy IRQ through it via MADT
 * overrides to the requested vector. Otherwise, falls back to unmasking
 * the legacy 8259 PIC.
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
