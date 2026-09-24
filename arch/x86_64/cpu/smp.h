/* ============================================================================
 * AzamiOS — Symmetric Multiprocessing (SMP) Subsystem Header
 * File: arch/x86_64/cpu/smp.h
 * ============================================================================ */
#pragma once

#include "../../../include/azami/types.h"
#include "idt.h" /* pt_regs_t */

#define SMP_MAX_CPUS  64

/* IPI vectors this kernel owns. Claimed explicitly in idt_init() so the
 * "fill the rest with isr_spurious" sweep leaves them alone. */
#define SMP_VEC_RESCHEDULE   49    /* "you have work / re-run the scheduler"  */
#define SMP_VEC_CALL_FUNC    250   /* run a function on this CPU (smp_call_*) */
#define SMP_VEC_TLB_FLUSH    251   /* TLB shootdown (arch/x86_64/mm/tlb.c)    */
#define SMP_VEC_STOP         252   /* panic: park this CPU forever            */

/**
 * struct cpu_info — Per-CPU data structure stored in GS segment.
 * GS.base points directly to the start of this structure for current CPU.
 *
 * The first fields' byte offsets are part of the ABI: the SYSCALL entry stub
 * and smp_get_cpu()/smp_current_cpu_id() below reach them with GS-relative
 * literals, so nothing may be inserted above +0x48 without updating those.
 */
typedef struct cpu_info {
    u64          kernel_rsp0;       /* +0x00: Kernel stack for ring 3 -> ring 0 transitions */
    u64          user_rsp;          /* +0x08: Scratch slot to save ring 3 RSP on SYSCALL */
    u32          cpu_id;            /* +0x10: Logical CPU ID (0 to n-1) */
    u32          lapic_id;          /* +0x14: Local APIC ID from hardware/Limine */
    struct cpu_info *self;          /* +0x18: Self pointer for GS-relative verification */
    struct thread   *current_thread;/* +0x20: Currently executing thread on this CPU */
    struct thread   *idle_thread;   /* +0x28: Per-CPU idle loop thread */
    u64          ticks;             /* +0x30: Timer ticks elapsed on this CPU */
    bool         is_bsp;            /* +0x38: True if Bootstrap Processor */
    struct thread   *prev_thread;   /* +0x40: Thread that just switched out, waiting for cleanup */
    bool         needs_reschedule;  /* +0x48: Deferred reschedule flag (set by sched_tick, checked on IRQ return) */

    /* ── Below here the layout is private to C code ───────────────────── */
    bool         online;            /* CPU has finished bringing itself up  */
    u64          ipis_sent;         /* IPI counters for /proc/interrupts    */
    u64          ipis_resched;
    u64          ipis_call;
    u64          ipis_tlb;
    u64          irq_count;         /* device interrupts taken on this CPU  */
} cpu_info_t;

/** smp_init() — Initialize per-CPU structures and boot Application Processors. */
void smp_init(void);

/** smp_get_cpu() — Get pointer to cpu_info_t for the current CPU via GS. */
static inline cpu_info_t *smp_get_cpu(void)
{
    cpu_info_t *cpu;
    __asm__ volatile("mov %%gs:0x18, %0" : "=r"(cpu));
    return cpu;
}

/** smp_current_cpu_id() — Return logical CPU ID of the calling CPU. */
static inline u32 smp_current_cpu_id(void)
{
    u32 id;
    __asm__ volatile("mov %%gs:0x10, %0" : "=r"(id));
    return id;
}

/** smp_cpu_count() — Return total number of CPUs the kernel manages. */
u32 smp_cpu_count(void);

/** smp_cpu_info(cpu_id) — per-CPU block for an arbitrary CPU, or NULL. */
cpu_info_t *smp_cpu_info(u32 cpu_id);

/** smp_online_mask() — bit k set for every CPU that has completed bring-up
 *  and is accepting IPIs. */
u64 smp_online_mask(void);

/** smp_cpu_online(cpu_id) — membership test against smp_online_mask(). */
bool smp_cpu_online(u32 cpu_id);

/** smp_cpu_apic_id(cpu_id) — APIC ID of a logical CPU (0 if unknown). */
u32 smp_cpu_apic_id(u32 cpu_id);

/**
 * smp_ap_stack_phys(cpu_id, out_base, out_len) — Return the physical range
 * backing an Application Processor's kernel stack (as allocated by
 * smp_init()). Used by kexec (kernel/kexec.c) to mark a still-parked AP's
 * stack reserved in the memory map handed to a freshly kexec'd kernel
 * instance, so its allocator does not hand that live (if permanently
 * dormant) memory out to something else.
 *
 * Returns false for cpu_id 0 (the BSP has no separate "AP stack" — see
 * smp_init()) or any cpu_id that is not currently online.
 */
bool smp_ap_stack_phys(u32 cpu_id, phys_addr_t *out_base, size_t *out_len);

/** smp_send_reschedule(cpu_id) — Send IPI to force rescheduling on a remote CPU. */
void smp_send_reschedule(u32 cpu_id);

/** smp_send_ipi(cpu_id, vector) — Send an arbitrary fixed IPI to a remote CPU.
 *  A no-op for the calling CPU itself and for ids that are not online. */
void smp_send_ipi(u32 cpu_id, u8 vector);

/** smp_send_ipi_mask(mask, vector) — Send @vector to every online CPU in
 *  @mask except the caller. Uses the all-but-self broadcast shorthand when
 *  @mask covers everyone, which is one ICR write instead of one per CPU. */
void smp_send_ipi_mask(u64 mask, u8 vector);

/** smp_send_ipi_allbutself(vector) — Broadcast to every other online CPU. */
void smp_send_ipi_allbutself(u8 vector);

/* ── Cross-CPU function calls ─────────────────────────────────────────────
 *
 * The mechanism a kernel needs whenever a change is only expressible as
 * "execute this on that core": programming an MSR that is architecturally
 * per-CPU (mitigation control, PAT, performance counters), re-arming a local
 * timer, invalidating a per-CPU cache, quiescing cores before a hardware
 * reconfiguration. Without it the kernel can only ever change the state of
 * whichever CPU it happens to be running on, which is why so much per-CPU
 * setup here had to be duplicated into the AP bring-up path.
 *
 * Requests are queued onto the target's lock-free list and collected by the
 * SMP_VEC_CALL_FUNC handler. A waiting sender drains its *own* queue while it
 * spins, so two CPUs calling into each other simultaneously cannot deadlock.
 */

/** smp_call_function_single(cpu, func, info, wait) — run @func(@info) on @cpu.
 *  @wait blocks until it has run. Returns 0, or a negative errno when @cpu is
 *  not online. Running on the calling CPU itself simply calls @func inline. */
int smp_call_function_single(u32 cpu, void (*func)(void *), void *info, bool wait);

/** smp_call_function_mask(mask, func, info, wait) — run @func(@info) on every
 *  online CPU in @mask. The calling CPU runs it inline if its own bit is set. */
int smp_call_function_mask(u64 mask, void (*func)(void *), void *info, bool wait);

/** smp_call_function(func, info, wait) — run @func(@info) on every online CPU
 *  except the caller. */
int smp_call_function(void (*func)(void *), void *info, bool wait);

/** smp_call_function_all(func, info, wait) — including the caller. */
int smp_call_function_all(void (*func)(void *), void *info, bool wait);

/** smp_call_function_interrupt() — SMP_VEC_CALL_FUNC handler. Called from
 *  isr_dispatch(); not for general use. */
void smp_call_function_interrupt(void);

/**
 * smp_stop_other_cpus() — bring every other CPU to a halt.
 *
 * Used by the panic path. A fixed IPI is tried first so a healthy CPU parks
 * cleanly; CPUs that have not acknowledged within the timeout are then hit
 * with an NMI, which is delivered even to a core spinning with interrupts
 * disabled — precisely the state a CPU that caused the panic is likely to be
 * in. Safe to call from any context, including from inside an exception.
 */
void smp_stop_other_cpus(void);

/** smp_nmi_stop_self() — NMI handler hook: park this CPU if a stop is in
 *  progress. Returns true when it consumed the NMI (and does not return at
 *  all, since it parks). */
bool smp_nmi_stop_self(void);

/** Set to true by the BSP once early init is complete; APs spin on this before
 *  starting their LAPIC timer and entering the scheduler (see smp.c). */
extern volatile bool g_smp_sched_active;
