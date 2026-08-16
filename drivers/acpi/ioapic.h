/* ============================================================================
 * AzamiOS — IO APIC Driver
 * File: drivers/acpi/ioapic.h
 * ============================================================================ */
#pragma once
#include "../../include/azami/types.h"

/* IO APIC Registers (via IOREGSEL/IOWIN) */
#define IOAPIC_REG_ID       0x00
#define IOAPIC_REG_VER      0x01
#define IOAPIC_REG_ARB      0x02
#define IOAPIC_REG_REDTBL0  0x10

/** ioapic_init() — Initialise the IO APIC by parsing MADT overrides. */
void ioapic_init(void);

/** ioapic_set_irq() — Route a legacy IRQ to a specific vector and APIC ID. */
void ioapic_set_irq(u8 irq, u8 vector, u32 lapic_id);

/** ioapic_mask_irq() — Mask a legacy IRQ. */
void ioapic_mask_irq(u8 irq);

/** ioapic_is_active() — Check if IO APIC took over from legacy PIC. */
bool ioapic_is_active(void);
