/* ============================================================================
 * AzamiOS — Local APIC Driver (xAPIC / x2APIC)
 * File: arch/x86_64/cpu/lapic.h
 * ============================================================================ */
#pragma once

#include "../../../include/azami/types.h"
#include "../../../include/azami/defs.h"
#include "msr.h"

/* LAPIC register offsets (MMIO, relative to lapic_base) */
#define LAPIC_ID            0x020
#define LAPIC_VERSION       0x030
#define LAPIC_TPR           0x080   /* Task Priority Register */
#define LAPIC_EOI           0x0B0   /* End of Interrupt */
#define LAPIC_SVR           0x0F0   /* Spurious Interrupt Vector Register */
#define LAPIC_ICR_LO        0x300   /* Interrupt Command Register low */
#define LAPIC_ICR_HI        0x310   /* Interrupt Command Register high */
#define LAPIC_LVT_TIMER     0x320
#define LAPIC_TIMER_INIT    0x380   /* Initial Count */
#define LAPIC_TIMER_CURR    0x390   /* Current Count */
#define LAPIC_TIMER_DIV     0x3E0   /* Divide Configuration */

/* SVR bits */
#define LAPIC_SVR_ENABLE    (1U << 8)   /* APIC Software Enable */
#define LAPIC_SVR_SPURIOUS  0xFF        /* Spurious vector = 255 */

/* LVT Timer modes */
#define LAPIC_TIMER_ONESHOT    0
#define LAPIC_TIMER_PERIODIC   (1U << 17)
#define LAPIC_TIMER_VECTOR     48        /* = vec 48 in IDT */
#define LAPIC_TIMER_DIV_16     0x3

/* ICR delivery modes */
#define LAPIC_ICR_INIT   0x00000500U
#define LAPIC_ICR_SIPI   0x00000600U
#define LAPIC_ICR_FIXED  0x00000000U
#define LAPIC_ICR_ASSERT (1U << 14)
#define LAPIC_ICR_LEVEL  (1U << 15)

/* ── Public API ──────────────────────────────────────────────────────────── */

/** lapic_init() — Map LAPIC MMIO, enable APIC via SVR, disable PIC. */
void lapic_init(void);

/** lapic_eoi() — Signal End-Of-Interrupt to the local APIC. */
void lapic_eoi(void);

/** lapic_id() — Return the LAPIC ID of the calling CPU. */
u32  lapic_id(void);

/** lapic_timer_calibrate() — Calibrate LAPIC timer against PIT/TSC. */
void lapic_timer_calibrate(void);

/** lapic_timer_start(hz) — Start periodic timer at the given frequency. */
void lapic_timer_start(u32 hz);

/** lapic_timer_stop() — Stop the LAPIC timer. */
void lapic_timer_stop(void);

/** lapic_send_ipi(lapic_id, vector) — Send a fixed IPI to one APIC. */
void lapic_send_ipi(u32 lapic_id, u8 vector);

/** lapic_send_sipi(lapic_id, trampoline_page) — Send Startup IPI to AP. */
void lapic_send_sipi(u32 lapic_id, u8 trampoline_page);

/** lapic_send_init(lapic_id) — Send INIT IPI to AP. */
void lapic_send_init(u32 lapic_id);

/** lapic_base_phys() — Return the physical base address of the LAPIC MMIO. */
phys_addr_t lapic_base_phys(void);
