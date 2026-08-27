/* ============================================================================
 * AzamiOS — CFS Scheduler & Process/Thread Management Implementation
 * File: kernel/sched/sched.c
 * ============================================================================ */

#define DEBUG 0
#include <azami/debug.h>
#include "sched.h"
#include "../mm/kmalloc.h"
#include "../mm/pmm.h"
#include "../object/object.h"
#include "../../arch/x86_64/mm/vmm.h"
#include "../../arch/x86_64/cpu/gdt.h"
#include "../../arch/x86_64/cpu/smp.h"
#include "../../arch/x86_64/cpu/spinlock.h"
#include "../../arch/x86_64/cpu/msr.h"
#include "../../drivers/char/console.h"
#include "../../include/azami/defs.h"
#include "../../fs/vfs.h"


static spinlock_t g_sched_lock = SPINLOCK_INIT;
static thread_t  *g_ready_queue = NULL;
static process_t *g_process_list = NULL;
static process_t *g_kernel_proc = NULL;
static u32 g_next_pid = 1;
static u32 g_next_tid = 1;

static void enqueue_ready(thread_t *t);

/* Sleep queue is kept sorted by sleep_end_ticks (ascending) for O(1) tick scan */
static thread_t *g_sleep_queue = NULL;
u64 g_system_ticks = 0;

static thread_t *g_idle_threads[16] = {NULL};

/* Per-CPU idle bitmask: bit i is set when CPU i is running its idle thread.
 * Allows enqueue_ready() to find an idle CPU in O(1) via __builtin_ctzll(). */
static volatile u64 g_idle_cpu_mask = 0;

static void sleep_queue_insert_sorted(thread_t *t); /* forward decl */

void sched_post_switch(void)
{
    cpu_info_t *cpu = smp_get_cpu();
    if (!cpu) return;

    if (cpu->current_thread && cpu->current_thread->proc) {
        wrmsr(MSR_FS_BASE, cpu->current_thread->proc->fs_base);
        wrmsr(MSR_KERNEL_GS_BASE, cpu->current_thread->proc->gs_base);
    } else {
        wrmsr(MSR_FS_BASE, 0);
        wrmsr(MSR_KERNEL_GS_BASE, 0);
    }

    if (cpu->prev_thread) {
        irqflags_t flags = spinlock_lock_irqsave(&g_sched_lock);
        thread_t *prev = cpu->prev_thread;
        cpu->prev_thread = NULL;

        if (prev->state == THREAD_DYING) {
            prev->state = THREAD_ZOMBIE;
            if (prev->proc && prev->proc != g_kernel_proc) {
                bool all_dead = true;
                for (thread_t *t = prev->proc->threads; t; t = t->proc_next) {
                    if (t->state != THREAD_ZOMBIE) {
                        all_dead = false;
                        break;
                    }
                }
                if (all_dead) {
                    prev->proc->is_zombie = true;
                    if (prev->proc->parent && prev->proc->parent->wait_thread) {
                        enqueue_ready(prev->proc->parent->wait_thread);
                        prev->proc->parent->wait_thread = NULL;
                    }
                }
            }
        } else if (prev->state == THREAD_READY) {
            if (prev != g_idle_threads[cpu->cpu_id]) {
                enqueue_ready(prev);
            }
        } else if (prev->state == THREAD_SLEEPING_PENDING) {
            if (prev->unblock_pending || g_system_ticks >= prev->sleep_end_ticks) {
                prev->unblock_pending = false;
                enqueue_ready(prev);
            } else {
                prev->state = THREAD_SLEEPING;
                /* Insert into sorted sleep queue (PERF-02) */
                sleep_queue_insert_sorted(prev);
            }
        } else if (prev->state == THREAD_BLOCKED_PENDING) {
            if (prev->unblock_pending) {
                prev->unblock_pending = false;
                enqueue_ready(prev);
            } else {
                prev->state = THREAD_BLOCKED;
            }
        }
        spinlock_unlock_irqrestore(&g_sched_lock, flags);
    }
}

/* Telemetry Stats */
static u64 g_cpu_idle_ticks[16] = {0};
static u64 g_cpu_active_ticks[16] = {0};

u32 sched_get_process_count(void)
{
    u32 count = 0;
    irqflags_t irqf = spinlock_lock_irqsave(&g_sched_lock);
    process_t *curr = g_process_list;
    while (curr) {
        count++;
        curr = curr->next;
    }
    spinlock_unlock_irqrestore(&g_sched_lock, irqf);
    return count;
}

u64 sched_get_idle_ticks(u32 cpu_id)
{
    if (cpu_id >= 16) return 0;
    return g_cpu_idle_ticks[cpu_id];
}

u64 sched_get_active_ticks(u32 cpu_id)
{
    if (cpu_id >= 16) return 0;
    return g_cpu_active_ticks[cpu_id];
}

/* Context switch assembly stubs */
extern void switch_to_asm(u64 *old_rsp, u64 new_rsp);
extern void fpu_save_asm(void *fpu_state);
extern void fpu_restore_asm(const void *fpu_state);

thread_t *sched_current_thread(void)
{
    cpu_info_t *cpu = smp_get_cpu();
    return cpu ? cpu->current_thread : NULL;
}

process_t *sched_current_process(void)
{
    thread_t *t = sched_current_thread();
    return t ? t->proc : NULL;
}

static void enqueue_ready(thread_t *t)
{
    t->state = THREAD_READY;
    t->next = NULL;

    if (!g_ready_queue || t->vruntime < g_ready_queue->vruntime) {
        t->next = g_ready_queue;
        g_ready_queue = t;
    } else {
        thread_t *curr = g_ready_queue;
        while (curr->next && curr->next->vruntime <= t->vruntime) {
            curr = curr->next;
        }
        t->next = curr->next;
        curr->next = t;
    }

    /* Wake up an idle CPU using O(1) bitmask lookup instead of O(n) scan. */
    u64 idle_mask = __atomic_load_n(&g_idle_cpu_mask, __ATOMIC_RELAXED);
    u32 my_cpu    = smp_current_cpu_id();
    /* Clear our own bit so we don't IPI ourselves unnecessarily */
    if (my_cpu < 64) idle_mask &= ~(1ULL << my_cpu);
    if (idle_mask) {
        u32 idle_cpu = (u32)__builtin_ctzll(idle_mask);
        smp_send_reschedule(idle_cpu);
    }
}

static thread_t *dequeue_ready(void)
{
    if (!g_ready_queue) return NULL;
    thread_t *t = g_ready_queue;
    g_ready_queue = t->next;
    t->next = NULL;
    return t;
}

process_t *proc_create(const char *name, phys_addr_t pml4_phys)
{
    process_t *proc = (process_t *)kzalloc(sizeof(process_t));
    if (!proc) return NULL;

    irqflags_t irqf = spinlock_lock_irqsave(&g_sched_lock);
    proc->pid = g_next_pid++;
    spinlock_unlock_irqrestore(&g_sched_lock, irqf);

    /* New process starts as the sole member and leader of its own process
     * group and session; fork() overrides this by inheriting from the parent. */
    proc->pgid = proc->pid;
    proc->sid  = proc->pid;

    proc->pml4_phys = pml4_phys ? pml4_phys : vmm_kernel_space();
    for (int i = 0; name && name[i] && i < 31; i++) {
        proc->name[i] = name[i];
    }
    proc->cwd[0] = '/';
    proc->cwd[1] = '\0';
    proc->exit_code = 0;
    proc->is_zombie = false;
    proc->wait_thread = NULL;
    proc->umask = 022; /* POSIX-02: default file creation mask */

    irqf = spinlock_lock_irqsave(&g_sched_lock);
    proc->next = g_process_list;
    g_process_list = proc;
    spinlock_unlock_irqrestore(&g_sched_lock, irqf);

    return proc;
}

void proc_destroy(process_t *proc)
{
    if (!proc) return;

    irqflags_t irqf = spinlock_lock_irqsave(&g_sched_lock);
    process_t **pproc = &g_process_list;
    while (*pproc) {
        if (*pproc == proc) {
            *pproc = proc->next;
        } else {
            if ((*pproc)->parent == proc) {
                (*pproc)->parent = NULL;
            }
            pproc = &(*pproc)->next;
        }
    }
    spinlock_unlock_irqrestore(&g_sched_lock, irqf);

    /* Close IPC channels */
    extern void ipc_channel_close_all(process_t *proc);
    ipc_channel_close_all(proc);

    /* Unmap shared memory */
    extern void ipc_shmem_unmap_all(process_t *proc);
    ipc_shmem_unmap_all(proc);

    /* Close handles and open file descriptors */
    for (int i = 0; i < 64; i++) {
        if (proc->handle_table[i]) {
            vfs_close((file_t *)proc->handle_table[i]);
            proc->handle_table[i] = NULL;
        }
        if (proc->obj_handle_table[i]) {
            extern s64 az_handle_close(process_t *proc, s64 handle_id);
            az_handle_close(proc, i);
        }
    }

    extern void vma_reset(process_t *p);
    vma_reset(proc);

    if (proc->pml4_phys && proc->pml4_phys != vmm_kernel_space()) {
        vmm_destroy_space(proc->pml4_phys);
        proc->pml4_phys = 0;
    }
    kfree(proc);
}

static void idle_loop(void *arg);
extern void thread_entry_trampoline(void);

/* ── Guarded kernel stacks ──────────────────────────────────────────────────
 * Each thread's 16 KB ring-0 stack is carved from a dedicated kernel VA window
 * with one unmapped guard page directly below it, so a stack overflow takes an
 * immediate #PF (CR2 landing in the guard range) instead of silently
 * corrupting whatever the HHDM aliased underneath. Physical pages need not be
 * contiguous.
 *
 * VA slots are handed out MONOTONICALLY and never recycled: this kernel has no
 * cross-CPU TLB shootdown, so re-pointing a live kernel VA at a fresh physical
 * page would leave stale global TLB entries on other CPUs. A retired slot's
 * stale entries are harmless because nothing ever touches that VA again. When
 * the window is exhausted we fall back to a plain contiguous HHDM stack.
 *
 * The window lives inside the HHDM's PML4 entry (index 256), far past real RAM
 * but under a top-level table Limine populated before the APs started: adding
 * a *PML4* entry after CR3 load would need a shootdown to be seen elsewhere,
 * whereas entries below an already-present PML4 slot are picked up by any CPU
 * that faults on the range. */
#define KSTACK_PAGES        4                                  /* 16 KB usable */
#define KSTACK_GUARD_PAGES  1
#define KSTACK_SLOT_PAGES   (KSTACK_PAGES + KSTACK_GUARD_PAGES)
#define KSTACK_MAX_SLOTS    8192
#define KSTACK_AREA_BASE    (0xFFFF800000000000ULL + 0x7000000000ULL)
#define KSTACK_AREA_END     (KSTACK_AREA_BASE + \
                             (u64)KSTACK_MAX_SLOTS * KSTACK_SLOT_PAGES * PAGE_SIZE)

static spinlock_t g_kstack_lock = SPINLOCK_INIT;
static u64        g_kstack_next;   /* monotonic slot index */

/* Returns the VA of the lowest mapped stack page (the guard page sits at
 * base - PAGE_SIZE), or 0 on out-of-memory. */
static u64 kstack_alloc(void)
{
    irqflags_t f = spinlock_lock_irqsave(&g_kstack_lock);
    u64 slot = g_kstack_next;
    if (slot < KSTACK_MAX_SLOTS) g_kstack_next++;
    spinlock_unlock_irqrestore(&g_kstack_lock, f);

    if (slot >= KSTACK_MAX_SLOTS) {
        phys_addr_t phys = pmm_alloc_pages(KSTACK_PAGES);
        return phys ? (u64)PHYS_TO_VIRT(phys) : 0;
    }

    u64 slot_base  = KSTACK_AREA_BASE + slot * (u64)KSTACK_SLOT_PAGES * PAGE_SIZE;
    u64 stack_base = slot_base + (u64)KSTACK_GUARD_PAGES * PAGE_SIZE;

    for (int p = 0; p < KSTACK_PAGES; p++) {
        phys_addr_t phys = pmm_alloc_page();
        u64 va = stack_base + (u64)p * PAGE_SIZE;
        if (!phys || vmm_map(vmm_kernel_space(), va, phys, VMM_KERNEL_RW) != 0) {
            if (phys) pmm_free_page(phys);
            for (int q = 0; q < p; q++) {
                u64 rva = stack_base + (u64)q * PAGE_SIZE;
                phys_addr_t rp = vmm_translate(vmm_kernel_space(), rva);
                vmm_unmap(vmm_kernel_space(), rva);
                if (rp) pmm_free_page(rp);
            }
            return 0;
        }
        /* Zero the freshly mapped stack page (kzalloc-equivalent hygiene). */
        __builtin_memset((void *)va, 0, PAGE_SIZE);
    }
    return stack_base;
}

static void kstack_free(u64 stack_base)
{
    if (!stack_base) return;

    if (stack_base >= KSTACK_AREA_BASE && stack_base < KSTACK_AREA_END) {
        for (int p = 0; p < KSTACK_PAGES; p++) {
            u64 va = stack_base + (u64)p * PAGE_SIZE;
            phys_addr_t phys = vmm_translate(vmm_kernel_space(), va);
            vmm_unmap(vmm_kernel_space(), va);
            if (phys) pmm_free_page(phys);
        }
    } else {
        pmm_free_pages(VIRT_TO_PHYS(stack_base), KSTACK_PAGES);
    }
}

thread_t *thread_create_ex(process_t *proc, uintptr_t entry, uintptr_t arg, bool is_kernel, bool enqueue)
{
    thread_t *t = (thread_t *)kzalloc(sizeof(thread_t));
    if (!t) return NULL;

    irqflags_t irqf = spinlock_lock_irqsave(&g_sched_lock);
    t->tid = g_next_tid++;
    spinlock_unlock_irqrestore(&g_sched_lock, irqf);

    t->proc = proc ? proc : g_kernel_proc;
    t->state = THREAD_READY;
    t->vruntime = 0;
    t->priority = 10;
    
    /* Allocate a guarded 16 KB kernel stack for this thread */
    u64 kstack_base = kstack_alloc();
    if (!kstack_base) {
        kfree(t);
        return NULL;
    }
    t->kernel_stack_base = kstack_base;
    t->kernel_stack_top  = kstack_base + (KSTACK_PAGES * PAGE_SIZE);

    irqf = spinlock_lock_irqsave(&g_sched_lock);
    t->proc_next = t->proc->threads;
    t->proc->threads = t;
    spinlock_unlock_irqrestore(&g_sched_lock, irqf);

    /* Initialize default clean x87 / SSE control state */
    *(u16 *)&t->fpu_state.buffer[0] = 0x037F;  /* FCW default */
    *(u32 *)&t->fpu_state.buffer[24] = 0x1F80; /* MXCSR default */

    /* Build initial register context on the kernel stack */
    u64 *sp = (u64 *)t->kernel_stack_top;

    if (is_kernel) {
        /* Push initial stack frame matching switch_to_asm expectations:
         * RIP, RBP, RBX, R12, R13, R14, R15 */
        *(--sp) = (u64)(uintptr_t)thread_entry_trampoline; /* RIP */
        *(--sp) = 0; /* RBP */
        *(--sp) = 0; /* RBX */
        *(--sp) = 0; /* R12 */
        *(--sp) = 0; /* R13 */
        *(--sp) = (u64)arg; /* R14 -> passed to rdi in trampoline */
        *(--sp) = (u64)entry; /* R15 -> passed to rsi in trampoline */
    } else {
        /* User space thread creation builds pt_regs_t for iretq */
        pt_regs_t *user_frame = (pt_regs_t *)(t->kernel_stack_top - sizeof(pt_regs_t));
        __builtin_memset(user_frame, 0, sizeof(pt_regs_t));
        user_frame->cs = 0x23;       /* User code segment (GDT slot 4 = 0x20 | RPL3) */
        user_frame->ss = 0x1B;       /* User data segment (GDT slot 3 = 0x18 | RPL3) */
        user_frame->rflags = 0x202;  /* IF set */
        user_frame->rip = entry;
        user_frame->rsp = arg;       /* User stack */
        t->user_regs = user_frame;

        sp = (u64 *)user_frame;
        extern void user_thread_entry_trampoline(void);
        *(--sp) = (u64)(uintptr_t)user_thread_entry_trampoline; /* RIP stub */
        *(--sp) = 0; *(--sp) = 0; *(--sp) = 0; *(--sp) = 0; *(--sp) = 0; *(--sp) = 0;
    }

    t->kernel_rsp = (u64)(uintptr_t)sp;

    /* Enqueue if requested and not an idle thread */
    if (enqueue && !(is_kernel && entry == (uintptr_t)idle_loop)) {
        irqf = spinlock_lock_irqsave(&g_sched_lock);
        enqueue_ready(t);
        spinlock_unlock_irqrestore(&g_sched_lock, irqf);
    }

    return t;
}

thread_t *thread_create(process_t *proc, uintptr_t entry, uintptr_t arg, bool is_kernel)
{
    return thread_create_ex(proc, entry, arg, is_kernel, true);
}

void sched_enqueue_thread(thread_t *t)
{
    if (!t) return;
    irqflags_t irqf = spinlock_lock_irqsave(&g_sched_lock);
    enqueue_ready(t);
    spinlock_unlock_irqrestore(&g_sched_lock, irqf);
}

static void idle_loop(void *arg)
{
    (void)arg;
    cpu_info_t *cpu = smp_get_cpu();
    for (;;) {
        if (g_ready_queue) {
        /* Clear idle bit before yielding so enqueue_ready doesn't IPI us again */
        if (cpu && cpu->cpu_id < 64)
            __atomic_and_fetch((u64 *)&g_idle_cpu_mask, ~(1ULL << cpu->cpu_id), __ATOMIC_RELAXED);
        sched_yield();
        }
        /* Mark this CPU as idle so enqueue_ready can find and wake us */
        if (cpu && cpu->cpu_id < 64)
            __atomic_or_fetch((u64 *)&g_idle_cpu_mask, (1ULL << cpu->cpu_id), __ATOMIC_RELAXED);
        cpu_sti();
        cpu_hlt();
        /* After wakeup (HLT returns), clear idle bit immediately */
        if (cpu && cpu->cpu_id < 64)
            __atomic_and_fetch((u64 *)&g_idle_cpu_mask, ~(1ULL << cpu->cpu_id), __ATOMIC_RELAXED);
    }
}

static void sched_reaper_loop(void *arg)
{
    (void)arg;
    for (;;) {
        /* Sleep 100ms between reaper sweeps instead of busy-yielding (L-06) */
        sched_sleep(10);

        irqflags_t irqf = spinlock_lock_irqsave(&g_sched_lock);
        
        process_t **pproc = &g_process_list;
        while (*pproc) {
            process_t *proc = *pproc;
            if (proc == g_kernel_proc) {
                pproc = &proc->next;
                continue;
            }

            /* Check if ALL threads are zombies (must have at least one thread to be reaped) */
            if (!proc->threads) {
                pproc = &proc->next;
                continue;
            }

            bool all_zombie = true; /* Assume dead; loop falsifies if any thread is live */
            thread_t *t = proc->threads;
            while (t) {
                if (t->state != THREAD_ZOMBIE) {
                    all_zombie = false;
                    break;
                }
                t = t->proc_next;
            }

            if (all_zombie) {
                /* If it has a live parent waiting, keep it so waitpid can collect the exit status */
                if (proc->parent && !proc->parent->is_zombie) {
                    proc->is_zombie = true;
                    pproc = &proc->next;
                    continue;
                }

                /* Remove process from list */
                *pproc = proc->next;
                
                /* Orphan children to prevent dangling parent pointers */
                process_t *child = g_process_list;
                while (child) {
                    if (child->parent == proc) {
                        child->parent = NULL;
                    }
                    child = child->next;
                }
                
                /* Unlock safely while freeing detached process resources */
                spinlock_unlock_irqrestore(&g_sched_lock, irqf);
                
                t = proc->threads;
                while (t) {
                    thread_t *next_t = t->proc_next;
                    /* Free guarded kernel stack + thread struct */
                    kstack_free(t->kernel_stack_base);
                    kfree(t);
                    t = next_t;
                }
                proc->threads = NULL;
                
                /* Free VMM page tables, open handles, IPC channels, and process struct */
                proc_destroy(proc);
                
                irqf = spinlock_lock_irqsave(&g_sched_lock);
                pproc = &g_process_list;
            } else {
                /* Process still alive — clean up any individual zombie threads */
                thread_t **pt = &proc->threads;
                while (*pt) {
                    thread_t *curr_t = *pt;
                    if (curr_t->state == THREAD_ZOMBIE) {
                        *pt = curr_t->proc_next;
                        spinlock_unlock_irqrestore(&g_sched_lock, irqf);
                        
                        kstack_free(curr_t->kernel_stack_base);
                        kfree(curr_t);
                        
                        irqf = spinlock_lock_irqsave(&g_sched_lock);
                        pt = &proc->threads;
                    } else {
                        pt = &(*pt)->proc_next;
                    }
                }
                pproc = &proc->next;
            }
        }
        spinlock_unlock_irqrestore(&g_sched_lock, irqf);
    }
}

void sched_init(void)
{
    g_kernel_proc = proc_create("AzamiOS-Kernel", vmm_kernel_space());

    /* Create per-CPU idle threads — g_idle_threads is sized for 16 CPUs.
     * WARN-5 fix: guard against systems reporting more than 16 CPUs. */
    u32 cpu_count = smp_cpu_count();
    if (cpu_count > 16) {
        pr_debug("[SCHED] WARNING: %u CPUs detected, clamping idle threads to 16.\n", cpu_count);
        cpu_count = 16;
    }
    for (u32 i = 0; i < cpu_count; i++) {
        thread_t *idle = thread_create(g_kernel_proc, (uintptr_t)idle_loop, 0, true);
        if (idle) {
            idle->cpu_id = i;
            g_idle_threads[i] = idle;
        }
    }
    
    /* Spawn background zombie reaper thread */
    thread_create(g_kernel_proc, (uintptr_t)sched_reaper_loop, 0, true);
    
    pr_debug("[SCHED] CFS Scheduler and Process manager initialized.\n");
}

void sched_start(void)
{
    cpu_info_t *cpu = smp_get_cpu();
    if (!cpu) PANIC("sched_start called without cpu_info in GS!");

    cpu_cli();

    spinlock_lock(&g_sched_lock);
    thread_t *next = dequeue_ready();
    spinlock_unlock(&g_sched_lock);

    if (!next) {
        next = g_idle_threads[cpu->cpu_id];
    }

    spinlock_lock(&g_sched_lock);
    next->state = THREAD_RUNNING;
    cpu->current_thread = next;
    spinlock_unlock(&g_sched_lock);

    /* Set TSS kernel stack pointer for ring 3 -> ring 0 transitions */
    gdt_set_rsp0(cpu->cpu_id, next->kernel_stack_top);
    cpu->kernel_rsp0 = next->kernel_stack_top;

    /* Switch CR3 if necessary */
    if (next->proc && next->proc->pml4_phys && (read_cr3() & VMM_PHYS_MASK) != next->proc->pml4_phys) {
        vmm_switch(next->proc->pml4_phys);
    }

    fpu_restore_asm(&next->fpu_state);

    /* Jump into the first thread's stack */
    __asm__ volatile(
        "mov %0, %%rsp \n\t"
        "pop %%r15 \n\t"
        "pop %%r14 \n\t"
        "pop %%r13 \n\t"
        "pop %%r12 \n\t"
        "pop %%rbx \n\t"
        "pop %%rbp \n\t"
        "ret \n\t"
        : : "r"(next->kernel_rsp) : "memory"
    );
    __builtin_unreachable();
}

void sched_yield(void)
{
    cpu_info_t *cpu = smp_get_cpu();
    if (!cpu || !cpu->current_thread) return;

    irqflags_t irqf = spinlock_lock_irqsave(&g_sched_lock);

    thread_t *prev = cpu->current_thread;
    thread_t *next = dequeue_ready();

    if (!next) {
        next = g_idle_threads[cpu->cpu_id];
    }

    if (prev == next) {
        prev->state = THREAD_RUNNING;
        spinlock_unlock_irqrestore(&g_sched_lock, irqf);
        return;
    }

    prev->state = THREAD_READY;
    /* Enqueue moved to sched_post_switch to prevent SMP race */

    next->state = THREAD_RUNNING;
    cpu->current_thread = next;

    gdt_set_rsp0(cpu->cpu_id, next->kernel_stack_top);
    cpu->kernel_rsp0 = next->kernel_stack_top;

    if (next->proc && next->proc->pml4_phys && (read_cr3() & VMM_PHYS_MASK) != next->proc->pml4_phys) {
        vmm_switch(next->proc->pml4_phys);
    }
    if (next->proc) {
        wrmsr(MSR_FS_BASE, next->proc->fs_base);
        wrmsr(MSR_KERNEL_GS_BASE, next->proc->gs_base);
    } else {
        wrmsr(MSR_FS_BASE, 0);
        wrmsr(MSR_KERNEL_GS_BASE, 0);
    }

    cpu->prev_thread = prev;
    spinlock_unlock(&g_sched_lock);
    fpu_save_asm(&prev->fpu_state);
    fpu_restore_asm(&next->fpu_state);
    switch_to_asm(&prev->kernel_rsp, next->kernel_rsp);
    sched_post_switch();
    if (irqf & (1 << 9)) cpu_sti();
}

void sched_tick(pt_regs_t *regs)
{
    (void)regs;
    
    /* Acknowledge the timer interrupt immediately so the LAPIC can send more
     * even if we context switch away from this thread. */
    extern void lapic_eoi(void);
    lapic_eoi();

    cpu_info_t *cpu = smp_get_cpu();
    if (!cpu || !cpu->current_thread) return;

    u64 current_ticks;
    if (cpu->is_bsp) {
        current_ticks = __atomic_add_fetch(&g_system_ticks, 1, __ATOMIC_RELAXED);
        extern void net_poll(void);
        net_poll();
    } else {
        current_ticks = __atomic_load_n(&g_system_ticks, __ATOMIC_RELAXED);
    }

    cpu->ticks++;
    thread_t *curr = cpu->current_thread;
    curr->vruntime += curr->priority;

    if (curr->proc == g_kernel_proc || curr == g_idle_threads[cpu->cpu_id]) {
        if (cpu->cpu_id < 16) g_cpu_idle_ticks[cpu->cpu_id]++;
    } else {
        if (cpu->cpu_id < 16) g_cpu_active_ticks[cpu->cpu_id]++;
    }

    /* Wake up sleeping threads and check preemption */
    spinlock_lock(&g_sched_lock);
    
    /* Wake sleeping threads. Queue is sorted ascending by sleep_end_ticks so we
     * can early-exit as soon as we see a tick in the future (PERF-02). */
    while (g_sleep_queue && g_sleep_queue->sleep_end_ticks <= current_ticks) {
        thread_t *waking = g_sleep_queue;
        g_sleep_queue = waking->next;
        waking->next = NULL;
        enqueue_ready(waking);
    }

    bool should_preempt = (g_ready_queue && (curr == g_idle_threads[cpu->cpu_id] || g_ready_queue->vruntime < curr->vruntime));
    spinlock_unlock(&g_sched_lock);

    if (should_preempt) {
        cpu->needs_reschedule = true;
    }
}

void sched_check_reschedule(void)
{
    cpu_info_t *cpu = smp_get_cpu();
    if (!cpu) return;
    if (cpu->needs_reschedule) {
        cpu->needs_reschedule = false;
        sched_yield();
    }
}

void sched_block(thread_state_t new_state)
{
    cpu_info_t *cpu = smp_get_cpu();
    if (!cpu || !cpu->current_thread) return;

    /* BUG-17 fix: validate and apply the requested state instead of ignoring it.
     * Only THREAD_BLOCKED_PENDING and THREAD_SLEEPING_PENDING are valid here;
     * fall back to BLOCKED_PENDING for any unexpected value. */
    thread_state_t pending_state;
    if (new_state == THREAD_SLEEPING_PENDING) {
        pending_state = THREAD_SLEEPING_PENDING;
    } else {
        pending_state = THREAD_BLOCKED_PENDING;
    }

    irqflags_t irqf = spinlock_lock_irqsave(&g_sched_lock);

    thread_t *prev = cpu->current_thread;

    if (prev->state == THREAD_READY) {
        prev->state = THREAD_RUNNING;
        spinlock_unlock_irqrestore(&g_sched_lock, irqf);
        return;
    }

    prev->state = pending_state;
    prev->unblock_pending = false;
    barrier();

    thread_t *next = dequeue_ready();
    if (!next) {
        next = g_idle_threads[cpu->cpu_id];
    }

    if (prev == next) {
        prev->state = THREAD_RUNNING;
        spinlock_unlock_irqrestore(&g_sched_lock, irqf);
        return;
    }

    next->state = THREAD_RUNNING;
    cpu->current_thread = next;

    gdt_set_rsp0(cpu->cpu_id, next->kernel_stack_top);
    cpu->kernel_rsp0 = next->kernel_stack_top;

    if (next->proc && next->proc->pml4_phys && (read_cr3() & VMM_PHYS_MASK) != next->proc->pml4_phys) {
        vmm_switch(next->proc->pml4_phys);
    }

    cpu->prev_thread = prev;
    spinlock_unlock(&g_sched_lock);
    fpu_save_asm(&prev->fpu_state);
    fpu_restore_asm(&next->fpu_state);
    switch_to_asm(&prev->kernel_rsp, next->kernel_rsp);
    sched_post_switch();
    if (irqf & (1 << 9)) cpu_sti();
}

void sched_sleep(u64 ticks)
{
    if (ticks == 0) {
        sched_yield();
        return;
    }
    
    cpu_info_t *cpu = smp_get_cpu();
    if (!cpu || !cpu->current_thread) return;
    
    irqflags_t irqf = spinlock_lock_irqsave(&g_sched_lock);
    
    thread_t *prev = cpu->current_thread;
    prev->sleep_end_ticks = g_system_ticks + ticks;
    prev->state = THREAD_SLEEPING_PENDING;
    prev->unblock_pending = false;
    
    thread_t *next = dequeue_ready();
    if (!next) {
        next = g_idle_threads[cpu->cpu_id];
    }

    if (prev == next) {
        prev->state = THREAD_RUNNING;
        spinlock_unlock_irqrestore(&g_sched_lock, irqf);
        return;
    }
    
    next->state = THREAD_RUNNING;
    cpu->current_thread = next;
    
    gdt_set_rsp0(cpu->cpu_id, next->kernel_stack_top);
    cpu->kernel_rsp0 = next->kernel_stack_top;
    
    if (next->proc && next->proc->pml4_phys && (read_cr3() & VMM_PHYS_MASK) != next->proc->pml4_phys) {
        vmm_switch(next->proc->pml4_phys);
    }
    
    cpu->prev_thread = prev;
    spinlock_unlock(&g_sched_lock);
    fpu_save_asm(&prev->fpu_state);
    fpu_restore_asm(&next->fpu_state);
    switch_to_asm(&prev->kernel_rsp, next->kernel_rsp);
    sched_post_switch();
    if (irqf & (1 << 9)) cpu_sti();
}

/* Insert thread into sleep queue keeping it sorted ascending by sleep_end_ticks.
 * This lets sched_tick() early-exit as soon as it sees a future wakeup time. */
static void sleep_queue_insert_sorted(thread_t *t)
{
    if (!g_sleep_queue || t->sleep_end_ticks <= g_sleep_queue->sleep_end_ticks) {
        t->next = g_sleep_queue;
        g_sleep_queue = t;
        return;
    }
    thread_t *curr = g_sleep_queue;
    while (curr->next && curr->next->sleep_end_ticks <= t->sleep_end_ticks)
        curr = curr->next;
    t->next = curr->next;
    curr->next = t;
}

void sched_unblock(thread_t *t)
{
    if (!t) return;
    irqflags_t irqf = spinlock_lock_irqsave(&g_sched_lock);
    if (t->state == THREAD_BLOCKED || t->state == THREAD_SLEEPING) {
        /* If sleeping, remove from sleep queue */
        if (t->state == THREAD_SLEEPING) {
            thread_t *curr = g_sleep_queue;
            thread_t *prev = NULL;
            while (curr) {
                if (curr == t) {
                    if (prev) prev->next = curr->next;
                    else g_sleep_queue = curr->next;
                    break;
                }
                prev = curr;
                curr = curr->next;
            }
        }
        enqueue_ready(t);
    } else if (t->state == THREAD_BLOCKED_PENDING || t->state == THREAD_SLEEPING_PENDING) {
        t->unblock_pending = true;
    } else if (t->state == THREAD_RUNNING) {
        t->state = THREAD_READY; /* Signal sched_block to abort */
    }
    spinlock_unlock_irqrestore(&g_sched_lock, irqf);
}

void sched_exit_thread(void)
{
    cpu_info_t *cpu = smp_get_cpu();
    if (!cpu || !cpu->current_thread) {
        cpu_halt_loop();
        __builtin_unreachable();
    }

    thread_t *prev = cpu->current_thread;

    /* Pre-clean handles outside sched_lock if this is the last thread in the process */
    if (prev->proc && prev->proc != g_kernel_proc) {
        bool is_last = true;
        irqflags_t irqf_chk = spinlock_lock_irqsave(&g_sched_lock);
        for (thread_t *t = prev->proc->threads; t; t = t->proc_next) {
            if (t != prev && t->state != THREAD_DYING && t->state != THREAD_ZOMBIE) {
                is_last = false;
                break;
            }
        }
        spinlock_unlock_irqrestore(&g_sched_lock, irqf_chk);

        if (is_last) {
            for (int i = 0; i < 64; i++) {
                if (prev->proc->handle_table[i]) {
                    file_t *f = (file_t *)prev->proc->handle_table[i];
                    prev->proc->handle_table[i] = NULL;
                    vfs_close(f);
                }
                if (prev->proc->obj_handle_table[i]) {
                    az_object_t *obj = prev->proc->obj_handle_table[i];
                    prev->proc->obj_handle_table[i] = NULL;
                    az_object_dereference(obj);
                }
            }
        }
    }

    cpu_cli();
    spinlock_lock(&g_sched_lock);

    prev->state = THREAD_DYING;

    thread_t *next = dequeue_ready();
    if (!next) {
        next = g_idle_threads[cpu->cpu_id];
    }

    next->state = THREAD_RUNNING;
    cpu->current_thread = next;

    gdt_set_rsp0(cpu->cpu_id, next->kernel_stack_top);
    cpu->kernel_rsp0 = next->kernel_stack_top;

    if (next->proc && next->proc->pml4_phys && (read_cr3() & VMM_PHYS_MASK) != next->proc->pml4_phys) {
        vmm_switch(next->proc->pml4_phys);
    }

    cpu->prev_thread = prev;
    spinlock_unlock(&g_sched_lock);
    fpu_restore_asm(&next->fpu_state);
    switch_to_asm(&prev->kernel_rsp, next->kernel_rsp);
    sched_post_switch();
    /* Should never reach here — prev is ZOMBIE */
    cpu_halt_loop();
    __builtin_unreachable();
}

s64 sched_waitpid(s32 target_pid, int *status, int options)
{
    process_t *curr_proc = sched_current_process();
    if (!curr_proc) return -(s64)EPERM;

    for (;;) {
        irqflags_t irqf = spinlock_lock_irqsave(&g_sched_lock);
        bool has_children = false;
        process_t *zombie_child = NULL;

        process_t *p = g_process_list;
        while (p) {
            if (p->parent == curr_proc) {
                /* POSIX waitpid pid argument:
                 *   -1  : any child
                 *    0  : any child in the caller's process group
                 *  < -1 : any child in process group |pid|
                 *  > 0  : the child with that exact pid                */
                bool match;
                if (target_pid == -1)      match = true;
                else if (target_pid == 0)  match = (p->pgid == curr_proc->pgid);
                else if (target_pid < -1)  match = (p->pgid == (u32)(-target_pid));
                else                       match = ((s32)p->pid == target_pid);

                if (match) {
                    has_children = true;
                    if (p->is_zombie) {
                        zombie_child = p;
                        break;
                    }
                }
            }
            p = p->next;
        }

        if (zombie_child) {
            s32 child_pid = (s32)zombie_child->pid;
            int exit_val = zombie_child->exit_code;

            /* Remove zombie child from process list */
            process_t **pp = &g_process_list;
            while (*pp) {
                if (*pp == zombie_child) {
                    *pp = zombie_child->next;
                    break;
                }
                pp = &(*pp)->next;
            }
            spinlock_unlock_irqrestore(&g_sched_lock, irqf);

            if (status) {
                /* POSIX wait status: WIFSIGNALED when a signal killed the
                 * child (low 7 bits = WTERMSIG), otherwise WIFEXITED with
                 * WEXITSTATUS in bits 8-15. */
                if (zombie_child->term_signal)
                    *status = zombie_child->term_signal & 0x7f;
                else
                    *status = (exit_val & 0xFF) << 8;
            }

            /* Free child threads and process */
            thread_t *t = zombie_child->threads;
            while (t) {
                thread_t *next_t = t->proc_next;
                kstack_free(t->kernel_stack_base);
                kfree(t);
                t = next_t;
            }
            zombie_child->threads = NULL;
            proc_destroy(zombie_child);

            return (s64)child_pid;
        }

        if (!has_children) {
            spinlock_unlock_irqrestore(&g_sched_lock, irqf);
            return -(s64)ECHILD;
        }

        if (options & 1 /* WNOHANG */) {
            spinlock_unlock_irqrestore(&g_sched_lock, irqf);
            return 0;
        }

        /* Block parent until a child changes state */
        thread_t *curr_thread = sched_current_thread();
        if (curr_proc->wait_thread && curr_proc->wait_thread != curr_thread) {
            spinlock_unlock_irqrestore(&g_sched_lock, irqf);
            sched_yield();
            continue;
        }
        curr_proc->wait_thread = curr_thread;
        spinlock_unlock_irqrestore(&g_sched_lock, irqf);

        sched_block(THREAD_BLOCKED);

        irqf = spinlock_lock_irqsave(&g_sched_lock);
        if (curr_proc->wait_thread == curr_thread) {
            curr_proc->wait_thread = NULL;
        }
        spinlock_unlock_irqrestore(&g_sched_lock, irqf);
    }
}

s64 sched_kill_process(u32 pid, int sig)
{
    if (pid == 0) return -(s64)EINVAL;

    irqflags_t irqf = spinlock_lock_irqsave(&g_sched_lock);
    process_t *target = NULL;
    for (process_t *p = g_process_list; p; p = p->next) {
        if (p->pid == pid) {
            target = p;
            break;
        }
    }

    if (!target) {
        spinlock_unlock_irqrestore(&g_sched_lock, irqf);
        return -(s64)ESRCH;
    }

    if (sig == 0) {
        spinlock_unlock_irqrestore(&g_sched_lock, irqf);
        return 0; /* Signal 0: check existence */
    }

    if (sig > 0 && sig < _NSIG) {
        sighandler_t handler = target->sigactions[sig].sa_handler;
        if (handler == SIG_IGN) {
            spinlock_unlock_irqrestore(&g_sched_lock, irqf);
            return 0; /* Ignored signal */
        }
        if (handler == SIG_DFL) {
            if (sig == 17 /* SIGCHLD */ || sig == 23 /* SIGURG */ || sig == 28 /* SIGWINCH */ || sig == 18 /* SIGCONT */) {
                spinlock_unlock_irqrestore(&g_sched_lock, irqf);
                return 0; /* Default action is ignore */
            }
        } else {
            /* Custom handler registered */
            target->sig_pending |= (1ULL << sig);
            /* Wake up blocked/sleeping threads to handle signal */
            for (thread_t *t = target->threads; t; t = t->proc_next) {
                if (t->state == THREAD_BLOCKED || t->state == THREAD_SLEEPING) {
                    if (t->state == THREAD_SLEEPING) {
                        thread_t *curr_s = g_sleep_queue;
                        thread_t *prev_s = NULL;
                        while (curr_s) {
                            if (curr_s == t) {
                                if (prev_s) prev_s->next = curr_s->next;
                                else g_sleep_queue = curr_s->next;
                                break;
                            }
                            prev_s = curr_s;
                            curr_s = curr_s->next;
                        }
                    }
                    enqueue_ready(t);
                } else if (t->state == THREAD_RUNNING && t->cpu_id != smp_current_cpu_id()) {
                    smp_send_reschedule(t->cpu_id);
                }
            }
            spinlock_unlock_irqrestore(&g_sched_lock, irqf);
            return 0;
        }
    }

    /* Fatal default signal or unhandled fatal: terminate target threads.
     * Record the terminating signal so waitpid() can report WIFSIGNALED;
     * keep exit_code = 128+sig for the shell's $? convention. */
    target->term_signal = sig;
    target->exit_code = 128 + sig;

    for (thread_t *t = target->threads; t; t = t->proc_next) {
        if (t->state == THREAD_BLOCKED || t->state == THREAD_SLEEPING ||
            t->state == THREAD_BLOCKED_PENDING || t->state == THREAD_SLEEPING_PENDING) {
            /* Remove from sleep queue if sleeping */
            if (t->state == THREAD_SLEEPING) {
                thread_t *curr_s = g_sleep_queue;
                thread_t *prev_s = NULL;
                while (curr_s) {
                    if (curr_s == t) {
                        if (prev_s) prev_s->next = curr_s->next;
                        else g_sleep_queue = curr_s->next;
                        break;
                    }
                    prev_s = curr_s;
                    curr_s = curr_s->next;
                }
            }
            /* Transition directly to DYING so sched_post_switch won't re-enqueue */
            t->state = THREAD_DYING;
        } else if (t->state == THREAD_READY) {
            /* Remove from ready queue */
            thread_t *curr_r = g_ready_queue;
            thread_t *prev_r = NULL;
            while (curr_r) {
                if (curr_r == t) {
                    if (prev_r) prev_r->next = curr_r->next;
                    else g_ready_queue = curr_r->next;
                    break;
                }
                prev_r = curr_r;
                curr_r = curr_r->next;
            }
            t->state = THREAD_DYING;
        } else if (t->state == THREAD_RUNNING) {
            /* Running on another CPU — set DYING, IPI to force reschedule */
            t->state = THREAD_DYING;
            if (t->cpu_id != smp_current_cpu_id()) {
                smp_send_reschedule(t->cpu_id);
            }
        }
        /* THREAD_DYING / THREAD_ZOMBIE: already on the way out, leave as-is */
    }

    bool self_killed = (target == sched_current_process());
    spinlock_unlock_irqrestore(&g_sched_lock, irqf);

    if (self_killed) {
        sched_exit_thread();
        __builtin_unreachable();
    }
    return 0;
}

process_t *sched_get_process_list(void)
{
    return g_process_list;
}

static irqflags_t g_sched_proc_irqf[16];

void sched_lock(void)
{
    u32 cpu_id = smp_current_cpu_id();
    if (cpu_id >= 16) cpu_id = 0;
    g_sched_proc_irqf[cpu_id] = spinlock_lock_irqsave(&g_sched_lock);
}

void sched_unlock(void)
{
    u32 cpu_id = smp_current_cpu_id();
    if (cpu_id >= 16) cpu_id = 0;
    spinlock_unlock_irqrestore(&g_sched_lock, g_sched_proc_irqf[cpu_id]);
}

u64 sched_get_ticks(void)
{
    return __atomic_load_n(&g_system_ticks, __ATOMIC_RELAXED);
}

