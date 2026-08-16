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
static u32 g_next_pid = 1;
static u32 g_next_tid = 1;

static void enqueue_ready(thread_t *t);

static thread_t *g_sleep_queue = NULL;
u64 g_system_ticks = 0;

static thread_t *g_idle_threads[16] = {NULL};

void sched_post_switch(void)
{
    cpu_info_t *cpu = smp_get_cpu();
    if (cpu && cpu->prev_thread) {
        irqflags_t flags = spinlock_lock_irqsave(&g_sched_lock);
        if (cpu->prev_thread->state == THREAD_DYING) {
            cpu->prev_thread->state = THREAD_ZOMBIE;
        } else if (cpu->prev_thread->state == THREAD_READY) {
            if (cpu->prev_thread != g_idle_threads[cpu->cpu_id]) {
                enqueue_ready(cpu->prev_thread);
            }
        }
        cpu->prev_thread = NULL;
        spinlock_unlock_irqrestore(&g_sched_lock, flags);
    }
}

/* Kernel idle process container */
static process_t *g_kernel_proc = NULL;

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

/* Context switch assembly stub */
extern void switch_to_asm(u64 *old_rsp, u64 new_rsp);

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
        return;
    }

    thread_t *curr = g_ready_queue;
    while (curr->next && curr->next->vruntime <= t->vruntime) {
        curr = curr->next;
    }
    t->next = curr->next;
    curr->next = t;
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

    proc->pml4_phys = pml4_phys ? pml4_phys : vmm_kernel_space();
    for (int i = 0; name && name[i] && i < 31; i++) {
        proc->name[i] = name[i];
    }
    proc->cwd[0] = '/';
    proc->cwd[1] = '\0';
    proc->exit_code = 0;
    proc->is_zombie = false;
    proc->wait_thread = NULL;

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

    if (proc->pml4_phys && proc->pml4_phys != vmm_kernel_space()) {
        vmm_destroy_space(proc->pml4_phys);
        proc->pml4_phys = 0;
    }
    kfree(proc);
}

static void idle_loop(void *arg);
extern void thread_entry_trampoline(void);

thread_t *thread_create(process_t *proc, uintptr_t entry, uintptr_t arg, bool is_kernel)
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
    
    /* Allocate 16 KB kernel stack for this thread */
    phys_addr_t kstack_phys = pmm_alloc_pages(4);
    if (!kstack_phys) {
        kfree(t);
        return NULL;
    }

    irqf = spinlock_lock_irqsave(&g_sched_lock);
    t->proc_next = t->proc->threads;
    t->proc->threads = t;
    spinlock_unlock_irqrestore(&g_sched_lock, irqf);

    t->kernel_stack_top = (u64)PHYS_TO_VIRT(kstack_phys) + (4 * PAGE_SIZE);

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

    /* Idle threads are assigned directly to g_idle_threads[cpu_id]; all other threads get enqueued */
    if (!(is_kernel && entry == (uintptr_t)idle_loop)) {
        irqf = spinlock_lock_irqsave(&g_sched_lock);
        enqueue_ready(t);
        spinlock_unlock_irqrestore(&g_sched_lock, irqf);
    }

    return t;
}

static void idle_loop(void *arg)
{
    (void)arg;
    for (;;) {
        cpu_sti();
        cpu_hlt();
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

            /* Check if ALL threads are zombies */
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
                    /* Free kernel stack */
                    phys_addr_t phys_stack = VIRT_TO_PHYS(t->kernel_stack_top - (4 * PAGE_SIZE));
                    pmm_free_pages(phys_stack, 4);
                    /* Free thread struct */
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
                        
                        phys_addr_t phys_stack = VIRT_TO_PHYS(curr_t->kernel_stack_top - (4 * PAGE_SIZE));
                        pmm_free_pages(phys_stack, 4);
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

    /* Create per-CPU idle threads */
    for (u32 i = 0; i < smp_cpu_count(); i++) {
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

    cpu_cli();
    spinlock_lock(&g_sched_lock);

    thread_t *prev = cpu->current_thread;
    thread_t *next = dequeue_ready();

    if (!next) {
        next = g_idle_threads[cpu->cpu_id];
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

    cpu->prev_thread = prev;
    spinlock_unlock(&g_sched_lock);
    switch_to_asm(&prev->kernel_rsp, next->kernel_rsp);
    sched_post_switch();
    cpu_sti();
}

void sched_tick(pt_regs_t *regs)
{
    (void)regs;
    
    /* Acknowledge the timer interrupt immediately so the LAPIC can send more
     * even if we context switch away from this thread. */
    extern void lapic_eoi(void);
    lapic_eoi();

    u64 current_ticks = __atomic_add_fetch(&g_system_ticks, 1, __ATOMIC_RELAXED);

    cpu_info_t *cpu = smp_get_cpu();
    if (!cpu || !cpu->current_thread) return;

    cpu->ticks++;
    thread_t *curr = cpu->current_thread;
    curr->vruntime += curr->priority;

    if (curr->proc == g_kernel_proc) {
        if (cpu->cpu_id < 16) g_cpu_idle_ticks[cpu->cpu_id]++;
    } else {
        if (cpu->cpu_id < 16) g_cpu_active_ticks[cpu->cpu_id]++;
    }

    /* Wake up sleeping threads and check preemption — but do NOT call
     * sched_yield() from inside the ISR.  Set a deferred flag instead.
     * (Audit fix C-02: calling sched_yield from an ISR leaks the interrupt
     * stack frame because the iretq never executes on the preempted thread.) */
    spinlock_lock(&g_sched_lock);
    
    /* Wake up sleeping threads */
    thread_t *curr_sleep = g_sleep_queue;
    thread_t *prev_sleep = NULL;
    while (curr_sleep) {
        if (current_ticks >= curr_sleep->sleep_end_ticks) {
            thread_t *waking = curr_sleep;
            curr_sleep = curr_sleep->next;
            if (prev_sleep) {
                prev_sleep->next = curr_sleep;
            } else {
                g_sleep_queue = curr_sleep;
            }
            enqueue_ready(waking);
        } else {
            prev_sleep = curr_sleep;
            curr_sleep = curr_sleep->next;
        }
    }

    bool should_preempt = (g_ready_queue && g_ready_queue->vruntime < curr->vruntime);
    spinlock_unlock(&g_sched_lock);

    if (should_preempt) {
        /* Defer the context switch until after the ISR returns (C-02) */
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

    cpu_cli();
    spinlock_lock(&g_sched_lock);

    thread_t *prev = cpu->current_thread;

    /* C-01 fix: set the desired blocked state under the lock, then check if a
     * concurrent sched_unblock() already changed it back to THREAD_READY.
     * sched_unblock() only transitions THREAD_BLOCKED/THREAD_SLEEPING → THREAD_READY,
     * so if our CAS-like pattern detects READY, an unblock raced us and we abort. */
    prev->state = new_state;
    barrier();  /* Ensure the store is visible before the check */

    if (prev->state == THREAD_READY) {
        /* A concurrent sched_unblock() on another CPU transitioned us back
         * to READY between setting new_state and this check.  Abort blocking
         * to avoid a lost wakeup. */
        spinlock_unlock(&g_sched_lock);
        cpu_sti();
        return;
    }

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
    switch_to_asm(&prev->kernel_rsp, next->kernel_rsp);
    sched_post_switch();
    cpu_sti();
}

void sched_sleep(u64 ticks)
{
    if (ticks == 0) {
        sched_yield();
        return;
    }
    
    cpu_info_t *cpu = smp_get_cpu();
    if (!cpu || !cpu->current_thread) return;
    
    cpu_cli();
    spinlock_lock(&g_sched_lock);
    
    thread_t *prev = cpu->current_thread;
    prev->sleep_end_ticks = g_system_ticks + ticks;
    prev->next = g_sleep_queue;
    g_sleep_queue = prev;
    
    thread_t *next = dequeue_ready();
    if (!next) {
        next = g_idle_threads[cpu->cpu_id];
    }
    
    prev->state = THREAD_SLEEPING;
    next->state = THREAD_RUNNING;
    cpu->current_thread = next;
    
    gdt_set_rsp0(cpu->cpu_id, next->kernel_stack_top);
    cpu->kernel_rsp0 = next->kernel_stack_top;
    
    if (next->proc && next->proc->pml4_phys && (read_cr3() & VMM_PHYS_MASK) != next->proc->pml4_phys) {
        vmm_switch(next->proc->pml4_phys);
    }
    
    cpu->prev_thread = prev;
    spinlock_unlock(&g_sched_lock);
    switch_to_asm(&prev->kernel_rsp, next->kernel_rsp);
    sched_post_switch();
    cpu_sti();
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

    cpu_cli();
    spinlock_lock(&g_sched_lock);

    thread_t *prev = cpu->current_thread;
    prev->state = THREAD_DYING;

    /* Check if this thread is the last non-zombie thread in the process */
    if (prev->proc) {
        bool last_thread = true;
        thread_t *t = prev->proc->threads;
        while (t) {
            if (t != prev && t->state != THREAD_DYING && t->state != THREAD_ZOMBIE) {
                last_thread = false;
                break;
            }
            t = t->proc_next;
        }
        if (last_thread) {
            prev->proc->is_zombie = true;

            /* Close handles if not already closed */
            for (int i = 0; i < 64; i++) {
                if (prev->proc->handle_table[i]) {
                    vfs_close((file_t *)prev->proc->handle_table[i]);
                    prev->proc->handle_table[i] = NULL;
                }
                if (prev->proc->obj_handle_table[i]) {
                    az_object_t *obj = prev->proc->obj_handle_table[i];
                    prev->proc->obj_handle_table[i] = NULL;
                    az_object_dereference(obj);
                }
            }

            /* Wake up parent if waiting */
            if (prev->proc->parent && prev->proc->parent->wait_thread) {
                enqueue_ready(prev->proc->parent->wait_thread);
                prev->proc->parent->wait_thread = NULL;
            }
        }
    }

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
                if (target_pid == -1 || (s32)p->pid == target_pid) {
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
                *status = (exit_val & 0xFF) << 8;
            }

            /* Free child threads and process */
            thread_t *t = zombie_child->threads;
            while (t) {
                thread_t *next_t = t->proc_next;
                phys_addr_t phys_stack = VIRT_TO_PHYS(t->kernel_stack_top - (4 * PAGE_SIZE));
                pmm_free_pages(phys_stack, 4);
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
        curr_proc->wait_thread = curr_thread;
        spinlock_unlock_irqrestore(&g_sched_lock, irqf);

        sched_block(THREAD_BLOCKED);
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

    /* Set exit code and mark zombie */
    target->exit_code = 128 + sig;
    target->is_zombie = true;

    /* Wake up waiting parent if any */
    if (target->parent && target->parent->wait_thread) {
        enqueue_ready(target->parent->wait_thread);
        target->parent->wait_thread = NULL;
    }

    /* Terminate target threads */
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
            t->state = THREAD_ZOMBIE;
        } else if (t->state == THREAD_READY) {
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
            t->state = THREAD_ZOMBIE;
        } else if (t->state == THREAD_RUNNING) {
            t->state = THREAD_DYING;
        }
    }

    spinlock_unlock_irqrestore(&g_sched_lock, irqf);
    return 0;
}

process_t *sched_get_process_list(void)
{
    return g_process_list;
}

void sched_lock(void)
{
    spinlock_lock(&g_sched_lock);
}

void sched_unlock(void)
{
    spinlock_unlock(&g_sched_lock);
}

u64 sched_get_ticks(void)
{
    return __atomic_load_n(&g_system_ticks, __ATOMIC_RELAXED);
}

