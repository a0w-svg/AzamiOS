/* ============================================================================
 * AzamiOS — Symmetric Multiprocessing (SMP) Subsystem Header
 * File: arch/x86_64/cpu/smp.h
 * ============================================================================ */
#pragma once

#include "../../../include/azami/types.h"
#include "idt.h" /* pt_regs_t */

#define SMP_MAX_CPUS  64

/**
 * struct cpu_info — Per-CPU data structure stored in GS segment.
 * GS.base points directly to the start of this structure for current CPU.
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

/** smp_cpu_count() — Return total number of online CPUs. */
u32 smp_cpu_count(void);

/** smp_send_reschedule(cpu_id) — Send IPI to force rescheduling on a remote CPU. */
void smp_send_reschedule(u32 cpu_id);
