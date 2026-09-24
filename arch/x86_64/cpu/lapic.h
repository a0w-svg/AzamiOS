/* ============================================================================
 * AzamiOS — Local APIC Driver (xAPIC / x2APIC)
 * File: arch/x86_64/cpu/lapic.h
 *
 * The same driver drives both APIC generations. Which one is in use is decided
 * once, on the BSP, by lapic_init(): x2APIC when the CPU enumerates it
 * (CPUID.1:ECX[21]) and the firmware has not locked it out, xAPIC otherwise.
 *
 * The distinction matters for more than register plumbing:
 *
 *   - x2APIC register access is a single RDMSR/WRMSR instead of an uncached
 *     MMIO store, so an IPI send is an order of magnitude cheaper. The ICR is
 *     one 64-bit MSR, which makes a send architecturally atomic and removes
 *     the CLI window xAPIC needs to keep a nested send from clobbering the
 *     destination half (see lapic_icr_guard_enter() in lapic.c).
 *   - APIC IDs are 32-bit. xAPIC tops out at 254 usable IDs, which is the
 *     hard ceiling on how many cores the machine can address at all.
 *   - Logical destination mode gets a flat 16-bit cluster ID, so a broadcast
 *     to "everyone but me" is one write rather than one per target — which is
 *     what makes TLB shootdown scale past a handful of cores.
 * ============================================================================ */
#pragma once

#include "../../../include/azami/types.h"
#include "../../../include/azami/defs.h"
#include "msr.h"

/* LAPIC register offsets (xAPIC MMIO byte offsets, relative to lapic_base).
 * In x2APIC mode the corresponding MSR is 0x800 + (offset >> 4). */
#define LAPIC_ID            0x020
#define LAPIC_VERSION       0x030
#define LAPIC_TPR           0x080   /* Task Priority Register */
#define LAPIC_PPR           0x0A0   /* Processor Priority Register */
#define LAPIC_EOI           0x0B0   /* End of Interrupt */
#define LAPIC_LDR           0x0D0   /* Logical Destination (xAPIC: writable) */
#define LAPIC_DFR           0x0E0   /* Destination Format (xAPIC only) */
#define LAPIC_SVR           0x0F0   /* Spurious Interrupt Vector Register */
#define LAPIC_ISR0          0x100   /* In-Service, 8 x 32 bits */
#define LAPIC_IRR0          0x200   /* Interrupt Request, 8 x 32 bits */
#define LAPIC_ESR           0x280   /* Error Status Register */
#define LAPIC_ICR_LO        0x300   /* Interrupt Command Register low */
#define LAPIC_ICR_HI        0x310   /* Interrupt Command Register high */
#define LAPIC_LVT_TIMER     0x320
#define LAPIC_LVT_THERMAL   0x330
#define LAPIC_LVT_PERF      0x340
#define LAPIC_LVT_LINT0     0x350
#define LAPIC_LVT_LINT1     0x360
#define LAPIC_LVT_ERROR     0x370
#define LAPIC_TIMER_INIT    0x380   /* Initial Count */
#define LAPIC_TIMER_CURR    0x390   /* Current Count */
#define LAPIC_TIMER_DIV     0x3E0   /* Divide Configuration */
#define LAPIC_SELF_IPI      0x3F0   /* x2APIC only: write vector to self-IPI */

/* x2APIC MSR range */
#define MSR_X2APIC_BASE     0x800U
#define MSR_X2APIC_ICR      0x830U   /* full 64-bit ICR in one MSR */
#define MSR_X2APIC_SELF_IPI 0x83FU

/* IA32_APIC_BASE bits */
#define APIC_BASE_BSP       (1ULL << 8)
#define APIC_BASE_EXTD      (1ULL << 10)  /* x2APIC mode enable */
#define APIC_BASE_ENABLE    (1ULL << 11)  /* APIC global enable */

/* SVR bits */
#define LAPIC_SVR_ENABLE    (1U << 8)   /* APIC Software Enable */
#define LAPIC_SVR_SPURIOUS  0xFF        /* Spurious vector = 255 */

/* LVT bits */
#define LAPIC_LVT_MASKED    (1U << 16)

/* LVT Timer modes */
#define LAPIC_TIMER_ONESHOT    0
#define LAPIC_TIMER_PERIODIC   (1U << 17)
#define LAPIC_TIMER_TSCDEADL   (1U << 18)
#define LAPIC_TIMER_VECTOR     48        /* = vec 48 in IDT */
#define LAPIC_TIMER_DIV_16     0x3

/* ICR delivery modes / flags */
#define LAPIC_ICR_FIXED  0x00000000U
#define LAPIC_ICR_LOWEST 0x00000100U
#define LAPIC_ICR_SMI    0x00000200U
#define LAPIC_ICR_NMI    0x00000400U
#define LAPIC_ICR_INIT   0x00000500U
#define LAPIC_ICR_SIPI   0x00000600U
#define LAPIC_ICR_LOGICAL (1U << 11)
#define LAPIC_ICR_ASSERT (1U << 14)
#define LAPIC_ICR_LEVEL  (1U << 15)
#define LAPIC_ICR_BUSY   (1U << 12)     /* xAPIC delivery-status bit */

/* Destination shorthands (ICR bits 19:18) */
#define LAPIC_ICR_DSH_NONE     0x00000000U
#define LAPIC_ICR_DSH_SELF     0x00040000U
#define LAPIC_ICR_DSH_ALL      0x00080000U
#define LAPIC_ICR_DSH_ALLBUT   0x000C0000U

/* ── Public API ──────────────────────────────────────────────────────────── */

/** lapic_init() — Enable this CPU's local APIC (x2APIC where available),
 *  program SVR/TPR/LVTs. Called once per CPU, BSP first. */
void lapic_init(void);

/** lapic_eoi() — Signal End-Of-Interrupt to the local APIC. */
void lapic_eoi(void);

/** lapic_id() — Return the APIC ID of the calling CPU (32-bit in x2APIC). */
u32  lapic_id(void);

/** lapic_x2apic_active() — true once the kernel has switched to x2APIC. */
bool lapic_x2apic_active(void);

/** lapic_timer_calibrate() — Calibrate LAPIC timer (and TSC) against HPET.
 *  Call once on the BSP before any lapic_timer_start(). */
void lapic_timer_calibrate(void);

/** lapic_timer_start(hz) — Start the periodic scheduler tick at @hz on the
 *  calling CPU. Uses TSC-deadline mode when the CPU supports it (one-shot
 *  under the hood; lapic_timer_rearm() re-arms it from the tick handler),
 *  otherwise the classic periodic LVT counter. */
void lapic_timer_start(u32 hz);

/** lapic_timer_rearm() — In TSC-deadline mode, program the next tick. A no-op
 *  in periodic mode. Must be called from the vector-48 handler. */
void lapic_timer_rearm(void);

/** lapic_timer_stop() — Stop the LAPIC timer on the calling CPU. */
void lapic_timer_stop(void);

/** lapic_tsc_khz() — Measured TSC frequency in kHz, 0 if unknown. */
u32  lapic_tsc_khz(void);

/** lapic_send_ipi(apic_id, vector) — Send a fixed IPI to one APIC. */
void lapic_send_ipi(u32 apic_id, u8 vector);

/** lapic_send_ipi_allbutself(vector) — One ICR write reaching every other
 *  CPU in the system. */
void lapic_send_ipi_allbutself(u8 vector);

/** lapic_send_ipi_self(vector) — Post an IPI to the calling CPU. */
void lapic_send_ipi_self(u8 vector);

/** lapic_send_nmi(apic_id) — Deliver an NMI to one APIC. Used to stop a CPU
 *  that is wedged with interrupts disabled (panic paths). */
void lapic_send_nmi(u32 apic_id);

/** lapic_send_nmi_allbutself() — NMI every other CPU. */
void lapic_send_nmi_allbutself(void);

/** lapic_send_sipi(apic_id, trampoline_page) — Send Startup IPI to AP. */
void lapic_send_sipi(u32 apic_id, u8 trampoline_page);

/** lapic_send_init(apic_id) — Send INIT IPI to AP. */
void lapic_send_init(u32 apic_id);

/** lapic_base_phys() — Return the physical base address of the LAPIC MMIO. */
phys_addr_t lapic_base_phys(void);

/** lapic_error_status() — Read-and-clear the LAPIC Error Status Register. */
u32 lapic_error_status(void);
