/* ============================================================================
 * AzamiOS — Legacy PIC (8259) Driver
 * File: arch/x86_64/cpu/pic.h / pic.c
 * ============================================================================ */
#pragma once
#include "../../../include/azami/types.h"

/* PIC I/O ports */
#define PIC1_CMD   0x20
#define PIC1_DATA  0x21
#define PIC2_CMD   0xA0
#define PIC2_DATA  0xA1

/* ICW1 / ICW4 constants */
#define PIC_ICW1_INIT  0x11
#define PIC_ICW4_8086  0x01
#define PIC_EOI        0x20

/**
 * pic_init(offset1, offset2) — Remap both PICs to the given vector offsets.
 *
 * On x86 the PIC starts at vectors 8 (IRQ0) and 70 (IRQ8) — colliding with
 * CPU exceptions. We remap them to offset1=32 (IRQ0→vec32) and offset2=40.
 * After an LAPIC is initialised, the PICs are masked entirely.
 */
void pic_init(u8 offset1, u8 offset2);

/** pic_mask_all() — Mask all IRQ lines on both PICs (used after LAPIC init). */
void pic_mask_all(void);

/** pic_eoi(irq) — Send End-Of-Interrupt to the relevant PIC. */
void pic_eoi(u8 irq);

/** pic_set_mask(irq) / pic_clear_mask(irq) — Mask or unmask a single IRQ. */
void pic_set_mask(u8 irq);
void pic_clear_mask(u8 irq);
