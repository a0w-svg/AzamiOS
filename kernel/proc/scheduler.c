/**
 * scheduler.c  –  Preemptive Round-Robin Process Scheduler (SMP-Safe)
 * Uses per-core cpu_data_t for context tracking. No shared g_active_context.
 * Runqueue is protected by g_sched_lock. Tasks tracked with on_cpu flag.
 */
#include "include/scheduler.h"
#include "include/process.h"
#include "../arch/include/gdt.h"
#include "../arch/include/isr.h"
#include "../arch/include/smp.h"
#include "../mem/include/paging.h"
#include "../mem/include/pmm.h"
#include "../klibc/include/stdio.h"
#include "../klibc/include/string.h"
#include "../arch/include/spinlock.h"
#include "../drivers/include/pit.h"

extern void switch_page_dir(void *page);
static uint32_t g_next_idle_pid = 0xF0000000; /* idle task PIDs start here, distinct from real procs */

static process_t *g_head = NULL;
static process_t *g_tail = NULL;
static bool g_sched_enabled = false;
static volatile int g_sched_lock = 0;

void scheduler_init(void) {
    g_head = NULL;
    g_tail = NULL;
    g_sched_enabled = false;
    g_sched_lock = 0;
    init_pit();
    kprintf("sched: SMP-safe preemptive scheduler initialized\n");
}

void scheduler_add(process_t *proc) {
    if (!proc) return;
    unsigned long flags;
    spinlock_acquire_irqsave(&g_sched_lock, &flags);
    if (!g_head) {
        g_head = proc;
        proc->next = proc;
        g_tail = proc;
    } else {
        g_tail->next = proc;
        proc->next = g_head;
        g_tail = proc;
    }
    g_sched_enabled = true;
    spinlock_release_irqrestore(&g_sched_lock, flags);
}

process_t *scheduler_get_current(void) {
    cpu_data_t *cpu = smp_get_current_cpu();
    return cpu->current_proc;
}

/* Idle task body - just halts, gets preempted when real work arrives */
static void idle_task_fn(void) {
    for (;;) {
        asm volatile("sti; hlt");
    }
}

process_t *scheduler_create_idle_task(uint32_t core_id) {
    process_t *p = (process_t*)pmm_alloc_block();
    if (!p) return NULL;
    memset(p, 0, sizeof(process_t));

    p->pid = g_next_idle_pid++;
    strncpy(p->name, "idle", 31);
    p->name[4] = '#';
    p->name[5] = '0' + (core_id % 10);
    p->name[6] = '\0';

    p->state = PROC_READY;
    p->ring = 0;
    p->on_cpu = false;
    p->cpu_id = core_id;

    uintptr_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    p->cr3 = cr3;

    void *kstack = pmm_alloc_block();
    if (!kstack) return NULL;
    p->kernel_stack = (uintptr_t)kstack;

    /* Build a minimal register frame pointing at idle_task_fn */
    uintptr_t *stack = (uintptr_t*)(p->kernel_stack + 4096);

    *(--stack) = 0x10;              /* SS  - kernel data */
    *(--stack) = p->kernel_stack + 4096; /* ESP */
    *(--stack) = 0x202;             /* EFLAGS (IF=1) */
    *(--stack) = 0x08;              /* CS  - kernel code 64-bit */
    *(--stack) = (uintptr_t)idle_task_fn; /* EIP/RIP */

    *(--stack) = 0;                 /* err_code */
    *(--stack) = 32;                /* int_no (IRQ0 slot) */

    /* General-purpose regs (all zero) */
    for (int i = 0; i < 8; i++) *(--stack) = 0;

    *(--stack) = 0x10;              /* DS - kernel data */

    p->kernel_esp = (uintptr_t)stack;
    return p;
}

void scheduler_schedule(void) {
    cpu_data_t *cpu = smp_get_current_cpu();
    unsigned long flags;
    spinlock_acquire_irqsave(&g_sched_lock, &flags);

    if (!g_sched_enabled || !cpu->active_context) {
        spinlock_release_irqrestore(&g_sched_lock, flags);
        return;
    }

    process_t *prev = cpu->current_proc;

    /* Save current context into the running process */
    if (prev) {
        prev->kernel_esp = cpu->active_context;
    }

    /* Walk the circular runqueue to find next READY task not running on any core */
    process_t *next = NULL;
    if (g_head) {
        process_t *candidate = prev ? prev->next : g_head;
        process_t *start = candidate;
        do {
            if (candidate->state == PROC_READY && !candidate->on_cpu) {
                next = candidate;
                break;
            }
            candidate = candidate->next;
        } while (candidate != start);
    }

    /* Fall back to idle task if nothing is runnable */
    if (!next) {
        next = cpu->idle_proc;
    }

    if (!next) {
        /* No idle proc yet - nothing to switch to */
        spinlock_release_irqrestore(&g_sched_lock, flags);
        return;
    }

    /* Release prev from this core */
    if (prev && prev != cpu->idle_proc) {
        prev->on_cpu = false;
        if (prev->state == PROC_RUNNING) {
            prev->state = PROC_READY;
        }
    }

    /* Claim next for this core */
    next->on_cpu = true;
    next->cpu_id = cpu->cpu_id;
    next->state = PROC_RUNNING;
    cpu->current_proc = next;
    cpu->active_context = next->kernel_esp;

    if (!prev || prev->pid != next->pid) {
        registers_t *r = (registers_t*)cpu->active_context;
        kprintf("sched[%u]: switch pid %u -> %u (rsp=0x%x rip=0x%x)\n",
                cpu->cpu_id,
                prev ? (unsigned)prev->pid : 0,
                (unsigned)next->pid,
                (uint32_t)cpu->active_context,
                (uint32_t)r->eip);
    }

    /* Update hardware TSS RSP0 so the next interrupt on this core saves to the right stack */
    set_kernel_stack(next->kernel_stack + 4096);

    /* Switch page tables if needed */
    uintptr_t cur_cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cur_cr3));
    if (cur_cr3 != next->cr3) {
        switch_page_dir((void*)(uintptr_t)next->cr3);
    }

    spinlock_release_irqrestore(&g_sched_lock, flags);
}
