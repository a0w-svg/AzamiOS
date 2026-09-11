/* ============================================================================
 * AzamiOS — CFS Scheduler & Process/Thread Management Implementation
 * File: kernel/sched/sched.c
 * ============================================================================ */

#define DEBUG 0
#include <azami/debug.h>
#include "sched.h"
#include "../ptrace.h"
#include "../syscall/syscall.h"   /* fd_table_release() */
#include "../perf/perf.h"
#include "../perf/ktrace.h"
#include "../security/security.h"
#include "../mm/kmalloc.h"
#include "../mm/pmm.h"
#include "../object/object.h"
#include "../../arch/x86_64/mm/vmm.h"
#include "../../arch/x86_64/cpu/gdt.h"
#include "../../arch/x86_64/cpu/smp.h"
#include "../../arch/x86_64/cpu/spinlock.h"
#include "../../arch/x86_64/cpu/msr.h"
#include "../../arch/x86_64/cpu/cpu.h"
#include "../../drivers/char/console.h"
#include "../../include/azami/defs.h"
#include "../../fs/vfs.h"
#include "../../arch/x86_64/cpu/hwaccel.h"


static spinlock_t g_sched_lock = SPINLOCK_INIT;
static thread_t  *g_ready_queue = NULL;
static process_t *g_process_list = NULL;
static process_t *g_kernel_proc = NULL;
static u32 g_next_pid = 1;
static u32 g_next_pcid = 0;   /* rolls 1..4095 for user address spaces */
static u32 g_next_tid = 1;

/* CFS min_vruntime: monotonically-advancing floor equal to the largest
 * vruntime ever dequeued as the run-queue head. Threads are enqueued with
 * their vruntime clamped up to this floor.
 *
 * Without it, a thread joining the queue with a stale-low vruntime — a brand
 * new thread (thread_create_ex sets vruntime = 0) or one that just woke from a
 * long sleep/block — sorts ahead of every running thread and keeps the CPU
 * until it accumulates their vruntime. On a box that has been up a while the
 * running threads sit at (ticks_run * priority), so "catching up" means many
 * seconds of exclusive CPU: new and just-woken threads starve everything else.
 *
 * Guarded by g_sched_lock (every enqueue_ready/dequeue_ready caller holds it). */
static u64 g_min_vruntime = 0;

static void enqueue_ready(thread_t *t);

/* Sleep queue is kept sorted by sleep_end_ticks (ascending) for O(1) tick scan */
static thread_t *g_sleep_queue = NULL;
u64 g_system_ticks = 0;

static thread_t *g_idle_threads[SMP_MAX_CPUS] = {NULL};

/* Per-CPU idle bitmask: bit i is set when CPU i is running its idle thread.
 * Allows enqueue_ready() to find an idle CPU in O(1) via __builtin_ctzll(). */
static volatile u64 g_idle_cpu_mask = 0;

static void sleep_queue_insert_sorted(thread_t *t); /* forward decl */
static void notify_waiter_locked(process_t *parent, int sig); /* forward decl */

void sched_post_switch(void)
{
    cpu_info_t *cpu = smp_get_cpu();
    if (!cpu) return;

    /* Close out the outgoing task's hardware counters and open the incoming
     * one's. This runs in the *new* thread on the core both of them ran on,
     * which is what makes the counter delta attributable to exactly one task.
     * Before the lock below, because cpu->prev_thread is cleared there. */
    perf_sched_switch(cpu->prev_thread ? cpu->prev_thread->proc : NULL,
                      cpu->current_thread ? cpu->current_thread->proc : NULL);

    /*
     * FS base and KERNEL_GS_BASE only matter to code that will run in ring 3:
     * a kernel thread uses neither (%gs in ring 0 is the per-CPU block, set
     * once at boot) and never sysrets to user, so its switch skips these
     * writes entirely — an MSR write and an FSGSBASE write saved on every
     * switch that lands on the idle thread or a kernel worker. The registers
     * keep whatever the last user thread left; the next user thread's switch
     * reloads them here.
     */
    if (cpu->current_thread && cpu->current_thread->proc &&
        cpu->current_thread->proc != g_kernel_proc) {
        /* Per-thread TLS base when the thread set one (CLONE_SETTLS); otherwise
         * the process-wide base (single-threaded / main thread). */
        thread_t *ct = cpu->current_thread;
        u64 base = ct->has_thread_fs_base ? ct->fs_base : ct->proc->fs_base;
        if (g_fsgsbase_enabled) wrfsbase(base);
        else wrmsr(MSR_FS_BASE, base);
        wrmsr(MSR_KERNEL_GS_BASE, ct->proc->gs_base);
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
                    notify_waiter_locked(prev->proc->parent, SIGCHLD);
                    if (prev->proc->tracer_pid && (!prev->proc->parent || prev->proc->parent->pid != prev->proc->tracer_pid)) {
                        for (process_t *tr = g_process_list; tr; tr = tr->next) {
                            if (tr->pid == prev->proc->tracer_pid) { notify_waiter_locked(tr, SIGCHLD); break; }
                        }
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
static u64 g_cpu_idle_ticks[SMP_MAX_CPUS] = {0};
static u64 g_cpu_active_ticks[SMP_MAX_CPUS] = {0};

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

/*
 * Collect the PIDs of live processes matching a setpriority(2)-style selector
 * into @out (capacity @max). @which is PRIO_PROCESS(0) / PRIO_PGRP(1) /
 * PRIO_USER(2); @who is the pid / pgid / real-uid (0 already resolved by the
 * caller to its own). Returns the number written (capped at @max). Snapshots
 * under g_sched_lock and returns bare pids so the caller can re-resolve each
 * with proc_get_by_pid() without holding the scheduler lock across the work.
 */
int sched_collect_pids(u32 *out, int max, int which, u32 who)
{
    if (!out || max <= 0) return 0;
    int n = 0;
    irqflags_t irqf = spinlock_lock_irqsave(&g_sched_lock);
    for (process_t *p = g_process_list; p && n < max; p = p->next) {
        bool match = (which == 0) ? (p->pid  == who)
                   : (which == 1) ? (p->pgid == who)
                   : (which == 2) ? (p->uid  == who)
                   : false;
        if (match) out[n++] = p->pid;
    }
    spinlock_unlock_irqrestore(&g_sched_lock, irqf);
    return n;
}

u64 sched_get_idle_ticks(u32 cpu_id)
{
    if (cpu_id >= SMP_MAX_CPUS) return 0;
    return g_cpu_idle_ticks[cpu_id];
}

u64 sched_get_active_ticks(u32 cpu_id)
{
    if (cpu_id >= SMP_MAX_CPUS) return 0;
    return g_cpu_active_ticks[cpu_id];
}

/* Context switch assembly stubs */
extern void switch_to_asm(u64 *old_rsp, u64 new_rsp);
extern void fpu_save_asm(void *fpu_state);
extern void fpu_restore_asm(const void *fpu_state);
extern void xsave_save_asm(void *area, u64 mask);
extern void xsave_restore_asm(const void *area, u64 mask);
extern void xsaveopt_save_asm(void *area, u64 mask);
extern void xsavec_save_asm(void *area, u64 mask);
extern u8   g_osxsave_enabled;      /* set in vmm_init() */
extern u8   g_xsave_variant;        /* XSAVE_VARIANT_*, chosen at boot */
extern u64  g_xcr0_mask;            /* XCR0 actually programmed; RFBM for XSAVE/XRSTOR */

/* SIMD context save/restore. When XSAVE is available we save exactly the state
 * components in XCR0 (x87 + SSE, plus AVX / AVX-512 / PKRU where the CPU has
 * them — see g_xcr0_mask); otherwise we fall back to FXSAVE (x87 + SSE only).
 *
 * Which save instruction we use is decided once at boot and never re-examined
 * per switch: XSAVEC packs the image and skips components the CPU has but we
 * did not enable, XSAVEOPT elides components untouched since the last restore
 * from this same address, and plain XSAVE writes the full standard layout.
 * All three are read back by XRSTOR, which distinguishes the layouts from the
 * XCOMP_BV bit the save instruction wrote — so the restore path is shared.
 *
 * The per-thread area is only 16-byte aligned by kmalloc, so we bump the
 * pointer up to the 64-byte boundary XSAVE requires. Passing a bit not in XCR0
 * as the XRSTOR RFBM is #GP, so the mask must be g_xcr0_mask, never a literal.
 *
 * The XSAVEOPT variant is only sound because a thread's area is allocated with
 * the thread and never reused for anything else: the modified-optimisation
 * tracks the *address* the state was last restored from. */
#define FPU_ALIGN(p)     ((void *)(((uintptr_t)(p) + 63) & ~(uintptr_t)63))

static inline void fpu_save(fpu_state_t *fs)
{
    void *a = FPU_ALIGN(fs->buffer);
    if (!g_osxsave_enabled) { fpu_save_asm(a); return; }
    switch (g_xsave_variant) {
        case XSAVE_VARIANT_XSAVEC:   xsavec_save_asm(a, g_xcr0_mask);   break;
        case XSAVE_VARIANT_XSAVEOPT: xsaveopt_save_asm(a, g_xcr0_mask); break;
        default:                     xsave_save_asm(a, g_xcr0_mask);    break;
    }
}

static inline void fpu_restore(const fpu_state_t *fs)
{
    void *a = FPU_ALIGN(fs->buffer);
    if (g_osxsave_enabled) xsave_restore_asm(a, g_xcr0_mask);
    else                   fpu_restore_asm(a);
}

/*
 * The kernel is built -mno-sse/-mno-mmx, so a kernel thread provably never
 * touches x87/SSE/AVX state: its save area stays at init and restoring it is
 * pointless. fpu_switch() skips both ends for a kernel thread, so an
 * idle-thread or kernel-worker switch — the common case on a lightly loaded
 * box — pays no XSAVE/XRSTOR (up to ~2.7 KiB of state traffic per direction)
 * at all, and a user->kernel->user round trip leaves the user thread's live
 * vector registers untouched. A user thread is still eager-saved on
 * switch-out, so one that migrates to another core finds its state where
 * XRSTOR expects it — laziness never crosses a CPU boundary.
 */
static inline bool thread_uses_fpu(const thread_t *t)
{
    return t && t->proc && t->proc != g_kernel_proc;
}

static inline void fpu_switch(thread_t *prev, thread_t *next)
{
    if (thread_uses_fpu(prev)) fpu_save(&prev->fpu_state);
    if (thread_uses_fpu(next)) fpu_restore(&next->fpu_state);
}

/* Prepare a fresh thread's SIMD area. A zeroed area is "init state" for the
 * x87/SSE/AVX *register* components under XRSTOR, but MXCSR is NOT covered by
 * XSTATE_BV: XRSTOR always reloads it from the legacy image (offset 24) when
 * the restore mask includes SSE/AVX. A zero MXCSR unmasks every SIMD FP
 * exception, so with CR4.OSXMMEXCPT set the first inexact mulsd/addsd in user
 * code traps as #XM. Seed FCW/MXCSR in the legacy image on both paths. */
static inline void fpu_area_init(fpu_state_t *fs)
{
    u8 *a = (u8 *)FPU_ALIGN(fs->buffer);
    __builtin_memset(fs->buffer, 0, sizeof fs->buffer);
    *(u16 *)(a + 0)  = 0x037F;    /* FCW:        default rounding, all masked */
    *(u32 *)(a + 24) = 0x1F80;    /* MXCSR:      all SIMD FP exceptions masked */
    *(u32 *)(a + 28) = 0x0000FFBF;/* MXCSR_MASK: standard writable-bit mask   */
}

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

    /* Clamp up to the run-queue floor so a stale-low vruntime cannot starve
     * the queue (see g_min_vruntime). This is a no-op for a thread that was
     * just preempted — it ran, so its vruntime already sits at or above the
     * floor — and only bites the new/just-woken case it is meant to fix. */
    if (t->vruntime < g_min_vruntime) t->vruntime = g_min_vruntime;

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
        /* Not __builtin_ctzll(): the kernel targets a baseline without BMI1,
         * so GCC can't fold that builtin into TZCNT and falls back to a
         * `bsf` sequence anyway — hw_ctz64() gets the same instruction (or
         * genuine TZCNT where the CPU has it) without going through the
         * builtin's UB-on-zero contract, which the surrounding `if` already
         * makes moot here but hw_ctz64() documents properly regardless. */
        u32 idle_cpu = hw_ctz64(idle_mask);
        smp_send_reschedule(idle_cpu);
    }
}

static thread_t *dequeue_ready(void)
{
    if (!g_ready_queue) return NULL;
    thread_t *t = g_ready_queue;
    g_ready_queue = t->next;
    t->next = NULL;
    /* The queue is sorted ascending, so the head is the minimum: advance the
     * floor to it. Monotone by construction — never walks backwards. */
    if (t->vruntime > g_min_vruntime) g_min_vruntime = t->vruntime;
    return t;
}

process_t *proc_create(const char *name, phys_addr_t pml4_phys)
{
    process_t *proc = (process_t *)kzalloc(sizeof(process_t));
    if (!proc) return NULL;

    proc->pml4_phys = pml4_phys ? pml4_phys : vmm_kernel_space();

    irqflags_t irqf = spinlock_lock_irqsave(&g_sched_lock);
    proc->pid = g_next_pid++;
    /* PCID: the kernel address space is always tag 0 (its pages are global,
     * shared by every context); each user address space gets a distinct tag,
     * recycled modulo the 12-bit space. pcid_primed starts clear so the first
     * switch to this space on each core is a flushing load — which is what
     * makes a recycled tag safe. */
    proc->pcid = (proc->pml4_phys == vmm_kernel_space())
                     ? 0
                     : (u16)(g_next_pcid++ % 4095u) + 1u;
    spinlock_unlock_irqrestore(&g_sched_lock, irqf);

    /* New process starts as the sole member and leader of its own process
     * group and session; fork() overrides this by inheriting from the parent. */
    proc->pgid = proc->pid;
    proc->sid  = proc->pid;
    for (int i = 0; name && name[i] && i < 31; i++) {
        proc->name[i] = name[i];
    }
    proc->cwd[0] = '/';
    proc->cwd[1] = '\0';
    proc->root[0] = '/';   /* unconfined until chroot(2) says otherwise */
    proc->root[1] = '\0';
    proc->exit_code = 0;
    proc->is_zombie = false;
    proc->last_cpu  = (u32)-1;   /* has not run anywhere yet */
    proc->wait_thread = NULL;
    proc->umask = 022; /* POSIX-02: default file creation mask */
    proc->fsuid = proc->euid;
    proc->fsgid = proc->egid;
    proc->pkey_alloc_map = 0x1; /* key 0 is the default key, always taken */

    /* POSIX resource limits: infinite unless a resource has a real ceiling.
     * fork() overwrites the whole table from the parent in proc_clone_attrs().
     * Index constants (RLIMIT_STACK = 3, RLIMIT_NPROC = 6, RLIMIT_NOFILE = 7,
     * RLIMIT_MEMLOCK = 8) match Linux and the syscall layer's RLIMIT_* macros. */
    for (int i = 0; i < RLIMIT_NLIMITS; i++) {
        proc->rlimits[i].rlim_cur = RLIM_INFINITY;
        proc->rlimits[i].rlim_max = RLIM_INFINITY;
    }
    proc->rlimits[3].rlim_cur = 8 * 1024 * 1024;          /* STACK  soft 8 MiB */
    proc->rlimits[4].rlim_cur = 0;                        /* CORE   soft 0     */
    proc->rlimits[6].rlim_cur = 1024;                     /* NPROC  soft       */
    proc->rlimits[6].rlim_max = 4096;                     /* NPROC  hard       */
    proc->rlimits[7].rlim_cur = PROC_MAX_FDS;             /* NOFILE soft = hard */
    proc->rlimits[7].rlim_max = PROC_MAX_FDS;             /*        (table size) */
    proc->rlimits[8].rlim_cur = 64 * 1024;                /* MEMLOCK soft 64 KiB */
    proc->rlimits[8].rlim_max = 64 * 1024;
    proc->rlimits[13].rlim_cur = 0;  proc->rlimits[13].rlim_max = 0;  /* NICE   */
    proc->rlimits[14].rlim_cur = 0;  proc->rlimits[14].rlim_max = 0;  /* RTPRIO */

    /* Seed the capability sets. kzalloc left euid == 0, so a plain new process
     * starts fully privileged exactly as it did before capabilities existed;
     * fork() immediately overwrites these from the parent in proc_clone_attrs(),
     * and execve() re-derives them in security_caps_on_exec(). */
    security_caps_init(proc, proc->pid <= 1);

    irqf = spinlock_lock_irqsave(&g_sched_lock);
    proc->next = g_process_list;
    g_process_list = proc;
    spinlock_unlock_irqrestore(&g_sched_lock, irqf);

    return proc;
}

static process_t *sched_find_reaper(process_t *child)
{
    process_t *p = child ? child->parent : NULL;
    while (p) {
        if (p->child_subreaper && !p->is_zombie) return p;
        p = p->parent;
    }
    process_t *curr = g_process_list;
    while (curr) {
        if ((curr->pid == 1 || curr->pid == 2) && !curr->is_zombie) return curr;
        curr = curr->next;
    }
    return NULL;
}

void proc_destroy(process_t *proc)
{
    if (!proc) return;

    /* Backstop for a process that never ran sys_exit_impl — one killed by a
     * signal, or torn down on a failed fork. Without this its tracees would
     * stay parked in a ptrace-stop with no tracer left to resume them. Must run
     * before the lock is taken: ptrace_release() takes it itself. */
    ptrace_release(proc);

    /* Likewise for any perf event still following this pid: its final count is
     * already complete (the last context switch away from it folded the delta
     * in), but the event must stop matching the pid before the number can be
     * handed to a new process. */
    perf_process_exit(proc);

    irqflags_t irqf = spinlock_lock_irqsave(&g_sched_lock);
    process_t *reaper = sched_find_reaper(proc);
    process_t **pproc = &g_process_list;
    while (*pproc) {
        if (*pproc == proc) {
            *pproc = proc->next;
        } else {
            if ((*pproc)->parent == proc) {
                (*pproc)->parent = reaper;
                if ((*pproc)->is_zombie && reaper && reaper->wait_thread) {
                    enqueue_ready(reaper->wait_thread);
                    reaper->wait_thread = NULL;
                }
            }
            pproc = &(*pproc)->next;
        }
    }
    spinlock_unlock_irqrestore(&g_sched_lock, irqf);

    /* @proc is now off g_process_list, so proc_get_by_pid() can no longer take
     * a new reference to it. One taken on another CPU just before the unlink
     * may still be live, though — wait the brief moment for it to drain before
     * anything below starts freeing what that caller is reading. */
    while (__atomic_load_n(&proc->hold_count, __ATOMIC_SEQ_CST))
        sched_yield();

    /* Close IPC channels */
    extern void ipc_channel_close_all(process_t *proc);
    ipc_channel_close_all(proc);

    /* Release System V IPC state (shm attachments, SEM_UNDO adjustments) and
     * any POSIX timers this process still owns. */
    extern void sysvipc_process_exit(process_t *proc);
    extern void ktimer_process_exit(process_t *proc);
    extern void mqueue_drop_proc(u32 pid);
    sysvipc_process_exit(proc);
    ktimer_process_exit(proc);
    /* A POSIX message queue holds one notification registration at a time.
     * Leaving a dead pid in it would lock every other process out of
     * mq_notify() on that queue for the lifetime of the system. */
    mqueue_drop_proc(proc->pid);

    /* POSIX: a reaped child's CPU time is charged to its parent. */
    if (proc->parent) {
        proc->parent->cutime_ticks += proc->utime_ticks + proc->cutime_ticks;
        proc->parent->cstime_ticks += proc->stime_ticks + proc->cstime_ticks;
    }

    /* Unmap shared memory */
    extern void ipc_shmem_unmap_all(process_t *proc);
    ipc_shmem_unmap_all(proc);

    /* Close handles and open file descriptors. Clears each slot under the
     * fd-table lock so a concurrent (possibly cross-process) fget() cannot race
     * a file_t into use as it is freed; also covers a late sys_exit /
     * sched_exit_thread cleanup without double-closing. */
    fd_table_release(proc);

    extern void vma_reset(process_t *p);
    vma_reset(proc);

    /* SECCOMP_MODE_FILTER: drop this process's reference to its (possibly
     * shared, fork()-inherited) filter chain. */
    extern void seccomp_filters_put(process_t *proc);
    seccomp_filters_put(proc);

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
 * VA slots are recycled through a small free list. That is only safe because
 * unmapping now performs a cross-CPU TLB shootdown (arch/x86_64/mm/tlb.c):
 * without one, re-pointing a live kernel VA at a fresh physical page would
 * leave other CPUs holding stale *global* translations to the old frame. When
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
static u64        g_kstack_next;   /* high-water mark for never-used slots */

/* Freed guarded-slot indices, recycled ahead of bumping g_kstack_next so a
 * long-lived system with high thread churn keeps getting guard-page-protected
 * stacks instead of falling through to the unguarded fallback. */
#define KSTACK_FREE_CACHE  1024
static u32 g_kstack_free_list[KSTACK_FREE_CACHE];
static u32 g_kstack_free_count;

/* Returns the VA of the lowest mapped stack page (the guard page sits at
 * base - PAGE_SIZE), or 0 on out-of-memory. */
static u64 kstack_alloc(void)
{
    irqflags_t f = spinlock_lock_irqsave(&g_kstack_lock);
    u64 slot;
    if (g_kstack_free_count > 0) {
        slot = g_kstack_free_list[--g_kstack_free_count];
        /* BUG-3 hardening: kstack_free() validates before pushing, but if memory
         * corruption has altered g_kstack_free_count or the array itself, catch
         * it here and fail loudly rather than silently using the unguarded fallback
         * which would map physical pages at an arbitrary kernel VA. */
        if (unlikely(slot >= KSTACK_MAX_SLOTS)) {
            /* BUG-N fix: a corrupt free-list entry must not silently degrade to an
             * unguarded stack — that defeats the entire guard-page safety model.
             * PANIC loudly so the memory corruption is surfaced immediately. */
            PANIC("[SCHED] BUG: corrupt kstack free-list entry %u — memory corruption detected",
                    (unsigned)slot);
        }
    } else {
        slot = g_kstack_next;
        if (slot < KSTACK_MAX_SLOTS) g_kstack_next++;
    }
    spinlock_unlock_irqrestore(&g_kstack_lock, f);

    if (slot >= KSTACK_MAX_SLOTS) {
        /* Unguarded fallback — should be unreachable now that slots recycle,
         * but still zero it so a stale-data leak can't ride along. */
        phys_addr_t phys = pmm_alloc_pages(KSTACK_PAGES);
        if (!phys) return 0;
        u64 base = (u64)PHYS_TO_VIRT(phys);
        hw_clear_pages((void *)base, (size_t)KSTACK_PAGES);
        return base;
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
        hw_clear_page((void *)va);
    }
    return stack_base;
}

static void kstack_free(u64 stack_base)
{
    if (!stack_base) return;

    if (stack_base >= KSTACK_AREA_BASE && stack_base < KSTACK_AREA_END) {
        /* Collect the frames first, then tear the whole stack down in one go:
         * vmm_unmap_range() shoots the range down across every CPU once,
         * rather than once per page. The frames are ours to release (the VMM
         * only auto-frees user pages), so they come back after that. */
        phys_addr_t phys[KSTACK_PAGES];
        for (int p = 0; p < KSTACK_PAGES; p++)
            phys[p] = vmm_translate(vmm_kernel_space(), stack_base + (u64)p * PAGE_SIZE);

        vmm_unmap_range(vmm_kernel_space(), stack_base, KSTACK_PAGES, false);

        for (int p = 0; p < KSTACK_PAGES; p++)
            if (phys[p]) pmm_free_page(phys[p]);
        /* Recycle the slot index (guard page stays unmapped). */
        u64 slot = (stack_base - (u64)KSTACK_GUARD_PAGES * PAGE_SIZE - KSTACK_AREA_BASE)
                   / ((u64)KSTACK_SLOT_PAGES * PAGE_SIZE);
        irqflags_t f = spinlock_lock_irqsave(&g_kstack_lock);
        if (slot < KSTACK_MAX_SLOTS && g_kstack_free_count < KSTACK_FREE_CACHE)
            g_kstack_free_list[g_kstack_free_count++] = (u32)slot;
        spinlock_unlock_irqrestore(&g_kstack_lock, f);
    } else {
        pmm_free_pages(VIRT_TO_PHYS(stack_base), KSTACK_PAGES);
    }
}

/*
 * Map a process's scheduling policy + nice value onto the CFS weight this
 * scheduler uses (thread->priority: vruntime += priority per tick, so a
 * smaller number is a bigger CPU share).
 *   - SCHED_FIFO / SCHED_RR  -> weight 1 (strongest share this design allows;
 *     not true run-to-completion, but RT tasks clearly dominate)
 *   - SCHED_IDLE             -> weight 40 (runs only when nothing else wants to)
 *   - otherwise              -> 10 + nice, clamped to [1, 39]
 */
u32 sched_weight_for(const process_t *proc)
{
    if (!proc) return 10;
    if (proc->sched_policy == 1 || proc->sched_policy == 2) return 1;   /* FIFO / RR */
    if (proc->sched_policy == 5) return 40;                             /* IDLE */
    s32 w = 10 + proc->prio_nice;
    if (w < 1)  w = 1;
    if (w > 39) w = 39;
    return (u32)w;
}

/* Re-apply the process's weight to every thread it currently has. Called after
 * setpriority()/sched_setscheduler() change the policy or nice value. */
void sched_apply_weight(process_t *proc)
{
    if (!proc) return;
    u32 w = sched_weight_for(proc);
    irqflags_t irqf = spinlock_lock_irqsave(&g_sched_lock);
    for (thread_t *t = proc->threads; t; t = t->proc_next)
        t->priority = w;
    spinlock_unlock_irqrestore(&g_sched_lock, irqf);
}

thread_t *thread_create_ex(process_t *proc, uintptr_t entry, uintptr_t arg, bool is_kernel, bool enqueue)
{
    thread_t *t = (thread_t *)kzalloc(sizeof(thread_t));
    if (!t) return NULL;

    irqflags_t irqf = spinlock_lock_irqsave(&g_sched_lock);
    t->tid = (proc && proc->threads == NULL) ? proc->pid : g_next_tid++;
    spinlock_unlock_irqrestore(&g_sched_lock, irqf);

    t->proc = proc ? proc : g_kernel_proc;
    t->state = THREAD_READY;
    t->vruntime = 0;
    /* Base CFS weight is 10 (vruntime grows by this each tick — lower means a
     * larger CPU share). A process that has changed its nice value or picked
     * an RT policy carries that onto every thread it spawns. */
    t->priority = sched_weight_for(t->proc);
    
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

    /* Initialize a clean x87 / SSE / AVX save area for this thread. */
    fpu_area_init(&t->fpu_state);

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
    bool have_id = cpu && cpu->cpu_id < 64;
    u64 mybit = have_id ? (1ULL << cpu->cpu_id) : 0;

    for (;;) {
        /* BUG-13: read g_ready_queue atomically — a plain load is a data race
         * on non-TSO ISAs (x86 TSO makes it safe today, but C UB travels). */
        if (__atomic_load_n(&g_ready_queue, __ATOMIC_ACQUIRE)) {
            /* Clear idle bit before yielding so enqueue_ready doesn't IPI us again */
            if (have_id)
                __atomic_and_fetch((u64 *)&g_idle_cpu_mask, ~mybit, __ATOMIC_RELAXED);
            sched_yield();
            continue;
        }

        /* Publish "CPU idle" so enqueue_ready() will send us a wakeup IPI. The
         * atomic OR is a full barrier on x86. */
        if (have_id)
            __atomic_or_fetch((u64 *)&g_idle_cpu_mask, mybit, __ATOMIC_RELAXED);

        /* Re-check with interrupts masked: a thread enqueued in the window
         * before our idle bit became visible could have had its wakeup IPI
         * skipped by enqueue_ready(). Catch it here instead of sleeping on it. */
        cpu_cli();
        /* Arm the wake-up watch on the run queue head *before* the final test,
         * so a thread enqueued in the gap still breaks us out of the wait. On a
         * CPU without MONITOR/MWAIT this is a no-op and the STI;HLT below
         * provides the same guarantee via the wake-up IPI. */
        cpu_idle_arm(&g_ready_queue);
        if (__atomic_load_n(&g_ready_queue, __ATOMIC_ACQUIRE)) {
            if (have_id)
                __atomic_and_fetch((u64 *)&g_idle_cpu_mask, ~mybit, __ATOMIC_RELAXED);
            cpu_sti();
            continue;
        }
        /* Parks the core until an interrupt or a store to the monitored line;
         * re-enables interrupts in the same uninterruptible window HLT needs. */
        cpu_idle_wait();

        if (have_id)
            __atomic_and_fetch((u64 *)&g_idle_cpu_mask, ~mybit, __ATOMIC_RELAXED);
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
                /* A syscall on another CPU holds a counted reference to this
                 * process. Leave it linked and its threads intact; a later
                 * sweep reaps it once the reference drops. (Read under
                 * g_sched_lock, which proc_get_by_pid() also holds while it
                 * raises the count.) */
                if (__atomic_load_n(&proc->hold_count, __ATOMIC_SEQ_CST)) {
                    pproc = &proc->next;
                    continue;
                }

                /* If it has a live parent waiting, keep it so waitpid can collect the exit status */
                if (proc->parent && !proc->parent->is_zombie) {
                    proc->is_zombie = true;
                    pproc = &proc->next;
                    continue;
                }

                /* Remove process from list */
                *pproc = proc->next;
                
                /* Reparent children to subreaper to prevent dangling parent pointers */
                process_t *reaper = sched_find_reaper(proc);
                process_t *child = g_process_list;
                while (child) {
                    if (child->parent == proc) {
                        child->parent = reaper;
                        if (child->is_zombie && reaper && reaper->wait_thread) {
                            enqueue_ready(reaper->wait_thread);
                            reaper->wait_thread = NULL;
                        }
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

    /* One idle thread per CPU. g_idle_threads is sized for SMP_MAX_CPUS; clamp
     * to that (the per-CPU idle bitmask is a u64, so SMP_MAX_CPUS <= 64). */
    u32 cpu_count = smp_cpu_count();
    if (cpu_count > SMP_MAX_CPUS) {
        pr_debug("[SCHED] WARNING: %u CPUs detected, clamping to %u.\n", cpu_count, SMP_MAX_CPUS);
        cpu_count = SMP_MAX_CPUS;
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
    next->cpu_id = cpu->cpu_id;   /* keep t->cpu_id live so a kill IPI can find it */
    spinlock_unlock(&g_sched_lock);

    /* Set TSS kernel stack pointer for ring 3 -> ring 0 transitions */
    gdt_set_rsp0(cpu->cpu_id, next->kernel_stack_top);
    cpu->kernel_rsp0 = next->kernel_stack_top;

    /* Switch CR3 if necessary */
    if (next->proc && next->proc->pml4_phys && (read_cr3() & VMM_PHYS_MASK) != next->proc->pml4_phys) {
        vmm_switch_proc(next->proc->pml4_phys, next->proc->pcid,
                        cpu->cpu_id, &next->proc->pcid_primed);
    }

    if (thread_uses_fpu(next)) fpu_restore(&next->fpu_state);

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
        if (prev->state != THREAD_DYING) prev->state = THREAD_RUNNING;
        spinlock_unlock_irqrestore(&g_sched_lock, irqf);
        return;
    }

    /* A thread marked THREAD_DYING by sched_kill_process (possibly from another
     * CPU) must keep that state through the switch so sched_post_switch turns it
     * into a ZOMBIE — never demote it back to READY/RUNNING. */
    if (prev->state != THREAD_DYING)
        prev->state = THREAD_READY;
    /* Enqueue moved to sched_post_switch to prevent SMP race */

    next->state = THREAD_RUNNING;
    cpu->current_thread = next;
    next->cpu_id = cpu->cpu_id;   /* keep t->cpu_id live so a kill IPI can find it */

    gdt_set_rsp0(cpu->cpu_id, next->kernel_stack_top);
    cpu->kernel_rsp0 = next->kernel_stack_top;

    /* Save the live FS_BASE for the outgoing thread.
     * Convention: has_thread_fs_base means the thread explicitly set its own
     * TLS base (via CLONE_SETTLS or arch_prctl), including to 0 — that case
     * used to be indistinguishable from "never set" when the check was
     * `fs_base != 0`, silently promoting an explicit FS_BASE=0 into
     * proc->fs_base. Fixed as TODO(T-01) by tracking the override with its
     * own flag instead of overloading the value. */
    if (prev && prev->proc) {
        u64 cur_fs = g_fsgsbase_enabled ? rdfsbase() : rdmsr(MSR_FS_BASE);
        if (prev->has_thread_fs_base) prev->fs_base = cur_fs;
        else prev->proc->fs_base = cur_fs;
    }

    if (next->proc && next->proc->pml4_phys && (read_cr3() & VMM_PHYS_MASK) != next->proc->pml4_phys) {
        vmm_switch_proc(next->proc->pml4_phys, next->proc->pcid,
                        cpu->cpu_id, &next->proc->pcid_primed);
    }

    /* The incoming thread's FS base and KERNEL_GS_BASE are programmed by
     * sched_post_switch(), which always runs on the far side of the switch
     * (interrupts stay masked until then, and a first-run thread reaches it
     * through the entry trampoline). This path used to write them here too —
     * pure duplication of ~3 register writes on every yield. */

    cpu->prev_thread = prev;
    spinlock_unlock(&g_sched_lock);
    fpu_switch(prev, next);
    switch_to_asm(&prev->kernel_rsp, next->kernel_rsp);
    sched_post_switch();
    if (irqf & (1 << 9)) cpu_sti();
}

void sched_tick(pt_regs_t *regs)
{
    
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

    KTRACE_CALL("sched_tick", current_ticks);

    cpu->ticks++;
    thread_t *curr = cpu->current_thread;
    curr->vruntime += curr->priority;

    if (curr->proc == g_kernel_proc || curr == g_idle_threads[cpu->cpu_id]) {
        if (cpu->cpu_id < SMP_MAX_CPUS) g_cpu_idle_ticks[cpu->cpu_id]++;
    } else {
        if (cpu->cpu_id < SMP_MAX_CPUS) g_cpu_active_ticks[cpu->cpu_id]++;

        /* POSIX process accounting: charge the tick to the running process,
         * split by the privilege level the timer interrupted. */
        if (curr->proc) {
            if ((regs->cs & 3) == 3) curr->proc->utime_ticks++;
            else                     curr->proc->stime_ticks++;
        }
    }

    /*
     * Lock-free pre-check. At 100 Hz on every core this is one of the busiest
     * would-be acquisitions of the global scheduler lock, and on the vast
     * majority of ticks there is nothing to do: no sleeper's deadline has
     * come up in this exact 10 ms window and the run queue is empty. Both
     * loads are racy on purpose — a wake missed here is picked up on the next
     * tick, and the preemption result only ever sets a hint that sched_yield()
     * re-derives under the lock — so a stale read costs at most one tick of
     * latency, never correctness.
     */
    thread_t *sq_head = __atomic_load_n(&g_sleep_queue, __ATOMIC_RELAXED);
    bool wake_due   = sq_head && sq_head->sleep_end_ticks <= current_ticks;
    bool rq_nonempty = __atomic_load_n(&g_ready_queue, __ATOMIC_RELAXED) != NULL;

    bool should_preempt = false;

    if (wake_due || rq_nonempty) {
        spinlock_lock(&g_sched_lock);

        /* Wake sleeping threads. Queue is sorted ascending by sleep_end_ticks
         * so we can early-exit as soon as we see a tick in the future. */
        while (g_sleep_queue && g_sleep_queue->sleep_end_ticks <= current_ticks) {
            thread_t *waking = g_sleep_queue;
            g_sleep_queue = waking->next;
            waking->next = NULL;
            enqueue_ready(waking);
        }

        should_preempt = (g_ready_queue && (curr == g_idle_threads[cpu->cpu_id] ||
                                            g_ready_queue->vruntime < curr->vruntime));
        spinlock_unlock(&g_sched_lock);
    }

    if (should_preempt || curr->state == THREAD_DYING) {
        cpu->needs_reschedule = true;
    }
}

void sched_check_reschedule(void)
{
    cpu_info_t *cpu = smp_get_cpu();
    if (!cpu) return;
    thread_t *cur = cpu->current_thread;
    /* Also yield unconditionally if the running thread has been marked DYING
     * (e.g. by SIGKILL from another CPU, delivered via the reschedule IPI):
     * it must leave the CPU so sched_post_switch can zombify it. */
    if (cpu->needs_reschedule || (cur && cur->state == THREAD_DYING)) {
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
        /* BUG-I fix: a concurrent sched_unblock() raced us and set state to
         * THREAD_READY before we could block.  Abort the block, but also clear
         * unblock_pending so a *future* sched_block() call isn't spuriously
         * skipped by a stale flag. */
        prev->state = THREAD_RUNNING;
        prev->unblock_pending = false;
        spinlock_unlock_irqrestore(&g_sched_lock, irqf);
        return;
    }

    /* Don't let a block request bury a pending kill — see sched_yield. */
    if (prev->state != THREAD_DYING) {
        prev->state = pending_state;
        prev->unblock_pending = false;
    }
    barrier();

    thread_t *next = dequeue_ready();
    if (!next) {
        next = g_idle_threads[cpu->cpu_id];
    }

    if (prev == next) {
        if (prev->state != THREAD_DYING) prev->state = THREAD_RUNNING;
        spinlock_unlock_irqrestore(&g_sched_lock, irqf);
        return;
    }

    next->state = THREAD_RUNNING;
    cpu->current_thread = next;
    next->cpu_id = cpu->cpu_id;   /* keep t->cpu_id live so a kill IPI can find it */

    gdt_set_rsp0(cpu->cpu_id, next->kernel_stack_top);
    cpu->kernel_rsp0 = next->kernel_stack_top;

    if (next->proc && next->proc->pml4_phys && (read_cr3() & VMM_PHYS_MASK) != next->proc->pml4_phys) {
        vmm_switch_proc(next->proc->pml4_phys, next->proc->pcid,
                        cpu->cpu_id, &next->proc->pcid_primed);
    }

    cpu->prev_thread = prev;
    spinlock_unlock(&g_sched_lock);
    fpu_switch(prev, next);
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
    /* Don't let a sleep request bury a pending kill — see sched_yield. */
    if (prev->state != THREAD_DYING) {
        prev->sleep_end_ticks = g_system_ticks + ticks;
        prev->state = THREAD_SLEEPING_PENDING;
        prev->unblock_pending = false;
    }

    thread_t *next = dequeue_ready();
    if (!next) {
        next = g_idle_threads[cpu->cpu_id];
    }

    if (prev == next) {
        if (prev->state != THREAD_DYING) prev->state = THREAD_RUNNING;
        spinlock_unlock_irqrestore(&g_sched_lock, irqf);
        return;
    }
    
    next->state = THREAD_RUNNING;
    cpu->current_thread = next;
    next->cpu_id = cpu->cpu_id;   /* keep t->cpu_id live so a kill IPI can find it */
    
    gdt_set_rsp0(cpu->cpu_id, next->kernel_stack_top);
    cpu->kernel_rsp0 = next->kernel_stack_top;
    
    if (next->proc && next->proc->pml4_phys && (read_cr3() & VMM_PHYS_MASK) != next->proc->pml4_phys) {
        vmm_switch_proc(next->proc->pml4_phys, next->proc->pcid,
                        cpu->cpu_id, &next->proc->pcid_primed);
    }
    
    cpu->prev_thread = prev;
    spinlock_unlock(&g_sched_lock);
    fpu_switch(prev, next);
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

/* Unlink @t from the sorted sleep queue if it is on it. Caller holds
 * g_sched_lock. Factored out because every path that has to make a sleeping
 * thread runnable early — unblock, kill, and now the job-control stop — needs
 * exactly this walk, and four hand-copied versions of it is four chances to
 * drop a node. */
static void sleep_queue_remove_locked(thread_t *t)
{
    thread_t *curr = g_sleep_queue, *prev = NULL;
    while (curr) {
        if (curr == t) {
            if (prev) prev->next = curr->next;
            else      g_sleep_queue = curr->next;
            curr->next = NULL;
            return;
        }
        prev = curr;
        curr = curr->next;
    }
}

void sched_unblock(thread_t *t)
{
    if (!t) return;
    irqflags_t irqf = spinlock_lock_irqsave(&g_sched_lock);
    if (t->state == THREAD_BLOCKED || t->state == THREAD_SLEEPING) {
        /* If sleeping, remove from sleep queue */
        if (t->state == THREAD_SLEEPING) sleep_queue_remove_locked(t);
        enqueue_ready(t);
    } else if (t->state == THREAD_BLOCKED_PENDING || t->state == THREAD_SLEEPING_PENDING) {
        t->unblock_pending = true;
    } else if (t->state == THREAD_RUNNING) {
        t->state = THREAD_READY; /* Signal sched_block to abort */
    }
    spinlock_unlock_irqrestore(&g_sched_lock, irqf);
}

/* ============================================================================
 * Job-control stop / resume
 *
 * A stopped process is not a new scheduler state: every one of its threads is
 * simply parked in THREAD_BLOCKED inside sched_stop_current()'s loop, with
 * thread_t::stopped set so the resume path can tell those threads apart from
 * ones blocked on I/O. Doing it this way means the stop rides on the same
 * block/unblock/kill machinery that is already exercised by every syscall,
 * instead of adding a state that all six switch statements in this file would
 * have to learn about — and SIGKILL keeps working on a stopped process for
 * free, because sched_kill_process() already handles THREAD_BLOCKED.
 *
 * The one thing a stop cannot do is interrupt kernel code that never returns
 * to ring 3. Threads park at the ring-3 boundary, so a thread that is inside a
 * blocking syscall stops only once that syscall returns. sched_request_stop()
 * therefore also ORs the stop signal into sig_pending: every blocking loop in
 * this kernel already breaks out on `sig_pending & ~sig_blocked` with -EINTR,
 * so that one line is what makes a process blocked in read(2) stoppable at all.
 * ========================================================================= */

/* Post @sig to @parent on behalf of @child and wake it out of wait4(2).
 * Caller holds g_sched_lock. */
static void notify_waiter_locked(process_t *parent, int sig)
{
    if (!parent || parent->is_zombie) return;

    /* SIGCHLD's default action is ignore, so only queue it when the parent
     * actually installed a handler — otherwise the bit would sit in
     * sig_pending forever and make every -EINTR test in the kernel fire. */
    if (sig > 0 && sig < _NSIG) {
        sighandler_t h = parent->sigactions[sig].sa_handler;
        if (h != SIG_IGN && h != SIG_DFL)
            __atomic_or_fetch(&parent->sig_pending, (1ULL << sig), __ATOMIC_SEQ_CST);
    }

    thread_t *w = parent->wait_thread;
    if (!w) return;
    parent->wait_thread = NULL;
    switch (w->state) {
    case THREAD_SLEEPING:
        sleep_queue_remove_locked(w);
        enqueue_ready(w);
        break;
    case THREAD_BLOCKED:
        enqueue_ready(w);
        break;
    case THREAD_BLOCKED_PENDING:
    case THREAD_SLEEPING_PENDING:
        w->unblock_pending = true;
        break;
    case THREAD_RUNNING:
        w->state = THREAD_READY;   /* make a racing sched_block() abort */
        break;
    default:
        break;
    }
}

void sched_notify_parent(process_t *p, int sig)
{
    if (!p) return;
    irqflags_t irqf = spinlock_lock_irqsave(&g_sched_lock);
    notify_waiter_locked(p->parent, sig);
    if (p->tracer_pid && (!p->parent || p->parent->pid != p->tracer_pid)) {
        for (process_t *tr = g_process_list; tr; tr = tr->next) {
            if (tr->pid == p->tracer_pid) { notify_waiter_locked(tr, sig); break; }
        }
    }
    spinlock_unlock_irqrestore(&g_sched_lock, irqf);
}

/* Nudge every thread of @p towards its next ring-3 exit. Caller holds the lock. */
static void poke_threads_locked(process_t *p)
{
    for (thread_t *t = p->threads; t; t = t->proc_next) {
        switch (t->state) {
        case THREAD_SLEEPING:
            sleep_queue_remove_locked(t);
            enqueue_ready(t);
            break;
        case THREAD_BLOCKED:
            enqueue_ready(t);
            break;
        case THREAD_BLOCKED_PENDING:
        case THREAD_SLEEPING_PENDING:
            t->unblock_pending = true;
            break;
        case THREAD_RUNNING:
            if (t->cpu_id != smp_current_cpu_id()) smp_send_reschedule(t->cpu_id);
            break;
        default:
            break;
        }
    }
}

static void request_stop_locked(process_t *p, int sig, u32 kind)
{
    if (!p || p->is_zombie) return;
    p->stop_signal   = sig;
    p->stop_notified = false;
    p->cont_pending  = false;
    __atomic_store_n(&p->stop_state, kind, __ATOMIC_RELEASE);

    /* POSIX: a stop discards a SIGCONT that has not been acted on yet. */
    __atomic_and_fetch(&p->sig_pending, ~(1ULL << SIGCONT), __ATOMIC_SEQ_CST);

    /* See the note above: this is what breaks a thread out of a blocking
     * syscall. sched_stop_current() clears the bit again as it parks, so the
     * signal is never also *delivered*. */
    if (sig > 0 && sig < _NSIG)
        __atomic_or_fetch(&p->sig_pending, (1ULL << sig), __ATOMIC_SEQ_CST);

    poke_threads_locked(p);
}

void sched_request_stop(process_t *p, int sig, u32 kind)
{
    if (!p) return;
    irqflags_t irqf = spinlock_lock_irqsave(&g_sched_lock);
    request_stop_locked(p, sig, kind);
    spinlock_unlock_irqrestore(&g_sched_lock, irqf);
}

bool sched_resume_process(process_t *p, bool cont_report)
{
    if (!p) return false;

    irqflags_t irqf = spinlock_lock_irqsave(&g_sched_lock);
    bool was_stopped = (p->stop_state != PROC_STOP_NONE);
    __atomic_store_n(&p->stop_state, PROC_STOP_NONE, __ATOMIC_RELEASE);
    p->stop_notified = false;

    /* POSIX: continuing discards every stop signal still pending. */
    __atomic_and_fetch(&p->sig_pending,
                       ~((1ULL << SIGSTOP) | (1ULL << SIGTSTP) |
                         (1ULL << 21 /*SIGTTIN*/) | (1ULL << 22 /*SIGTTOU*/)),
                       __ATOMIC_SEQ_CST);

    if (was_stopped && cont_report) p->cont_pending = true;

    /* Only the threads actually parked in the stop — waking a thread blocked
     * on I/O here would hand its syscall a spurious early return. */
    for (thread_t *t = p->threads; t; t = t->proc_next) {
        if (!t->stopped) continue;
        if (t->state == THREAD_BLOCKED)              enqueue_ready(t);
        else if (t->state == THREAD_BLOCKED_PENDING) t->unblock_pending = true;
        else if (t->state == THREAD_RUNNING)         t->state = THREAD_READY;
    }
    spinlock_unlock_irqrestore(&g_sched_lock, irqf);

    if (was_stopped && cont_report) sched_notify_parent(p, SIGCHLD);
    return was_stopped;
}

void sched_stop_current(void)
{
    process_t *p = sched_current_process();
    thread_t  *t = sched_current_thread();
    if (!p || !t) return;

    /* The signal that asked for the stop was only ever a wake-up device; it
     * must not also run its own disposition once we resume. */
    if (p->stop_signal > 0 && p->stop_signal < _NSIG)
        __atomic_and_fetch(&p->sig_pending, ~(1ULL << p->stop_signal),
                           __ATOMIC_SEQ_CST);

    while (__atomic_load_n(&p->stop_state, __ATOMIC_ACQUIRE) != PROC_STOP_NONE) {
        if (t->state == THREAD_DYING || p->is_zombie) break;
        t->stopped = true;
        barrier();
        /* Announce only once we are genuinely parked. A tracer woken before
         * that could PTRACE_GETREGS a frame the tracee is still running on,
         * and a tracer woken by a stop it then fails to observe would block in
         * wait4(2) with nothing left to wake it. */
        sched_notify_parent(p, SIGCHLD);
        sched_block(THREAD_BLOCKED_PENDING);
        barrier();
        t->stopped = false;
    }
    t->stopped = false;

    /* Woken by a fatal signal rather than a resume: sched_kill_process()
     * deliberately did not mark us THREAD_DYING (a parked, blocked thread that
     * is marked DYING never runs again), so the exit is ours to take. */
    if (p->term_signal != 0 && !p->is_zombie && t->state != THREAD_DYING)
        sched_exit_thread();
}

/* exit_group(2): mark every *other* thread of the current process DYING so the
 * whole process winds down, not just the caller. Blocked/sleeping siblings are
 * requeued so they get scheduled, notice DYING, and zombify; running siblings on
 * other CPUs are poked. The caller then falls through to its own thread exit.
 * Unlike sched_kill_process() this leaves term_signal alone — exit_group is a
 * normal (WIFEXITED) exit, not a signal death. */
void sched_exit_group_mark(void)
{
    cpu_info_t *cpu = smp_get_cpu();
    if (!cpu || !cpu->current_thread || !cpu->current_thread->proc) return;
    thread_t *self = cpu->current_thread;
    process_t *proc = self->proc;
    if (proc == g_kernel_proc) return;

    irqflags_t irqf = spinlock_lock_irqsave(&g_sched_lock);
    for (thread_t *t = proc->threads; t; t = t->proc_next) {
        if (t == self || t->state == THREAD_DYING || t->state == THREAD_ZOMBIE)
            continue;

        if (t->state == THREAD_RUNNING ||
            t->state == THREAD_BLOCKED_PENDING ||
            t->state == THREAD_SLEEPING_PENDING) {
            /* BUG-AH fix: On a CPU right now (or mid-switch in _PENDING) — can't
             * abandon or free its stack yet. Mark DYING and poke its CPU; it
             * zombifies itself on the way out via sched_post_switch and wakes waitpid. */
            t->state = THREAD_DYING;
            if (t->cpu_id != smp_current_cpu_id())
                smp_send_reschedule(t->cpu_id);
            continue;
        }

        /* Off every CPU (READY / BLOCKED / SLEEPING / *_PENDING). Its suspended
         * kernel stack will simply be abandoned — no CPU will switch_to it
         * again — so promote straight to ZOMBIE for the reaper to free. */
        if (t->state == THREAD_READY) {
            for (thread_t **pp = &g_ready_queue; *pp; pp = &(*pp)->next)
                if (*pp == t) { *pp = t->next; break; }
        } else if (t->state == THREAD_SLEEPING) {
            for (thread_t **pp = &g_sleep_queue; *pp; pp = &(*pp)->next)
                if (*pp == t) { *pp = t->next; break; }
        }
        t->state = THREAD_ZOMBIE;
    }
    spinlock_unlock_irqrestore(&g_sched_lock, irqf);
}

void sched_dethread_wait(void)
{
    thread_t *self = sched_current_thread();
    if (!self || !self->proc) return;

    for (u64 spins = 0; spins < 200000000ULL; spins++) {
        bool all_gone = true;
        irqflags_t irqf = spinlock_lock_irqsave(&g_sched_lock);
        for (thread_t *t = self->proc->threads; t; t = t->proc_next) {
            if (t != self && t->state != THREAD_ZOMBIE) { all_gone = false; break; }
        }
        spinlock_unlock_irqrestore(&g_sched_lock, irqf);
        if (all_gone) return;
        cpu_pause();
    }
    /* Timed out (should be unreachable). Proceeding is still safer than looping
     * forever; the stuck sibling is DYING and off userspace. */
}

void sched_dethread_reap(void)
{
    thread_t *self = sched_current_thread();
    if (!self || !self->proc) return;
    process_t *proc = self->proc;

    /* Collect dead sibling threads under the lock, then free them after
     * releasing it.  kstack_free() calls vmm_unmap_range() → tlb_shootdown_all()
     * which sends IPIs and spins waiting for remote CPUs to acknowledge.  Those
     * CPUs may be in a timer or reschedule ISR that tries to acquire g_sched_lock,
     * so holding that lock while waiting for them causes a deadlock. */
    thread_t *to_free = NULL;

    irqflags_t irqf = spinlock_lock_irqsave(&g_sched_lock);
    thread_t **tp = &proc->threads;
    while (*tp) {
        thread_t *t = *tp;
        if (t != self) {
            *tp = t->proc_next;
            /* Reuse proc_next as a temporary free-list link */
            t->proc_next = to_free;
            to_free = t;
        } else {
            tp = &t->proc_next;
        }
    }
    spinlock_unlock_irqrestore(&g_sched_lock, irqf);

    /* Now free stacks and thread structs without holding any lock. */
    while (to_free) {
        thread_t *next = to_free->proc_next;
        kstack_free(to_free->kernel_stack_base);
        kfree(to_free);
        to_free = next;
    }
}

void sched_exit_thread(void)
{
    cpu_info_t *cpu = smp_get_cpu();
    if (!cpu || !cpu->current_thread) {
        cpu_halt_loop();
        __builtin_unreachable();
    }

    thread_t *prev = cpu->current_thread;

    /* CLONE_CHILD_CLEARTID, while this thread's address space is still the
     * live one. musl's pthread_exit unlinks itself from the thread list under
     * __thread_list_lock and then relies on this write-and-wake to release it,
     * so skipping it wedges the next joiner rather than the exiting thread. */
    thread_clear_child_tid(prev);

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

        if (is_last)
            fd_table_release(prev->proc);   /* clears slots under the fd lock */
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
    next->cpu_id = cpu->cpu_id;   /* keep t->cpu_id live so a kill IPI can find it */

    gdt_set_rsp0(cpu->cpu_id, next->kernel_stack_top);
    cpu->kernel_rsp0 = next->kernel_stack_top;

    if (next->proc && next->proc->pml4_phys && (read_cr3() & VMM_PHYS_MASK) != next->proc->pml4_phys) {
        vmm_switch_proc(next->proc->pml4_phys, next->proc->pcid,
                        cpu->cpu_id, &next->proc->pcid_primed);
    }

    cpu->prev_thread = prev;
    spinlock_unlock(&g_sched_lock);
    if (thread_uses_fpu(next)) fpu_restore(&next->fpu_state);
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
        process_t *zombie_child  = NULL;
        process_t *stopped_child = NULL;
        process_t *cont_child    = NULL;

        process_t *p = g_process_list;
        while (p) {
            /* ptrace(2) makes the tracer a second waiter: it collects its
             * tracee's stops (and its death) even when it is not the parent. */
            bool is_tracee = (p->tracer_pid != 0 && p->tracer_pid == curr_proc->pid);
            if (p->parent == curr_proc || is_tracee) {
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
                    /* Only a process that has actually parked counts as
                     * stopped — see sched_stop_current(). A ptrace-stop is
                     * always reported to the tracer; a job-control stop needs
                     * WUNTRACED, exactly as Linux has it. */
                    if (!stopped_child && !p->stop_notified &&
                        p->stop_state != PROC_STOP_NONE &&
                        (is_tracee || (options & WUNTRACED))) {
                        for (thread_t *t = p->threads; t; t = t->proc_next) {
                            if (t->stopped) { stopped_child = p; break; }
                        }
                    }
                    if (!cont_child && p->cont_pending && (options & WCONTINUED))
                        cont_child = p;
                }
            }
            p = p->next;
        }

        if (zombie_child &&
            __atomic_load_n(&zombie_child->hold_count, __ATOMIC_SEQ_CST)) {
            /* A syscall on another CPU holds a counted reference to this child.
             * Do not unlink or free it yet — behave as if it were not reapable
             * on this pass. */
            spinlock_unlock_irqrestore(&g_sched_lock, irqf);
            if (options & WNOHANG) return 0;
            sched_yield();
            continue;
        }

        if (zombie_child) {
            s32 child_pid = (s32)zombie_child->pid;
            int exit_val  = zombie_child->exit_code;
            /* BUG-1 hardening: snapshot volatile fields while the lock is still
             * held. zombie_child is already unlinked from g_process_list before
             * the lock drops, so the reaper cannot race us — but snapshotting
             * here makes the invariant explicit and survives future refactoring
             * that might store process pointers outside the list (e.g. a pid
             * hash table) and thus re-open the race window. */
            int term_sig  = zombie_child->term_signal;

            /* Remove zombie child from process list */
            process_t **pp = &g_process_list;
            while (*pp) {
                if (*pp == zombie_child) {
                    *pp = zombie_child->next;
                    break;
                }
                pp = &(*pp)->next;
            }
            /* BUG-Q fix: verify every thread is genuinely ZOMBIE before freeing.
             * proc->is_zombie is set by sched_post_switch only after all threads
             * have transitioned to THREAD_ZOMBIE and no CPU holds prev_thread on
             * any of them, so this check should always pass — but be defensive
             * against future code changes that relax that invariant. */
            thread_t *t = zombie_child->threads;
            bool safe_to_free = true;
            while (t) {
                if (t->state != THREAD_ZOMBIE) { safe_to_free = false; break; }
                t = t->proc_next;
            }
            if (!safe_to_free) {
                /* A thread hasn't finished its context switch yet; yield and
                 * retry so we don't UAF a kernel stack still in use on another CPU. */
                spinlock_unlock_irqrestore(&g_sched_lock, irqf);
                sched_yield();
                continue;
            }
            spinlock_unlock_irqrestore(&g_sched_lock, irqf);

            if (status) {
                /* POSIX wait status: WIFSIGNALED when a signal killed the
                 * child (low 7 bits = WTERMSIG), otherwise WIFEXITED with
                 * WEXITSTATUS in bits 8-15. */
                if (term_sig)
                    *status = term_sig & 0x7f;
                else
                    *status = (exit_val & 0xFF) << 8;
            }

            /* Free child threads and process */
            t = zombie_child->threads;
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

        if (stopped_child) {
            s32 child_pid = (s32)stopped_child->pid;
            /* Linux wait-status encoding for a stop: 0x7f in the low byte,
             * the signal in the next. A ptrace event rides in the byte above
             * that, which is how PTRACE_EVENT_EXEC and friends are told apart
             * from a plain SIGTRAP stop. */
            int sig = (stopped_child->stop_state == PROC_STOP_PTRACE)
                      ? stopped_child->ptrace_stop_sig : stopped_child->stop_signal;
            int ev  = (stopped_child->stop_state == PROC_STOP_PTRACE)
                      ? (int)stopped_child->ptrace_event : 0;
            if (sig <= 0) sig = SIGSTOP;
            stopped_child->stop_notified = true;
            spinlock_unlock_irqrestore(&g_sched_lock, irqf);
            if (status) *status = (((sig & 0xff) | (ev << 8)) << 8) | 0x7f;
            return (s64)child_pid;
        }

        if (cont_child) {
            s32 child_pid = (s32)cont_child->pid;
            cont_child->cont_pending = false;
            spinlock_unlock_irqrestore(&g_sched_lock, irqf);
            if (status) *status = 0xffff;   /* WIFCONTINUED */
            return (s64)child_pid;
        }

        if (!has_children) {
            spinlock_unlock_irqrestore(&g_sched_lock, irqf);
            return -(s64)ECHILD;
        }

        if (options & WNOHANG) {
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

        sched_block(THREAD_BLOCKED_PENDING); /* BUG-X fix: must be _PENDING variant */

        irqf = spinlock_lock_irqsave(&g_sched_lock);
        if (curr_proc->wait_thread == curr_thread) {
            curr_proc->wait_thread = NULL;
        }
        spinlock_unlock_irqrestore(&g_sched_lock, irqf);
    }
}

process_t *sched_get_process_by_pid(u32 pid)
{
    if (pid == 0) return NULL;
    irqflags_t irqf = spinlock_lock_irqsave(&g_sched_lock);
    process_t *target = NULL;
    for (process_t *p = g_process_list; p; p = p->next) {
        if (p->pid == pid) {
            target = p;
            break;
        }
    }
    spinlock_unlock_irqrestore(&g_sched_lock, irqf);
    return target;
}

process_t *proc_get_by_pid(u32 pid)
{
    if (pid == 0) return NULL;
    irqflags_t irqf = spinlock_lock_irqsave(&g_sched_lock);
    process_t *target = NULL;
    for (process_t *p = g_process_list; p; p = p->next) {
        if (p->pid == pid) { target = p; break; }
    }
    /* A zombie is on its way out; callers that want to act on a process never
     * want one, and refusing here keeps every teardown path's hold_count check
     * on the simple side. */
    if (target && target->is_zombie) target = NULL;
    if (target) __atomic_add_fetch(&target->hold_count, 1, __ATOMIC_SEQ_CST);
    spinlock_unlock_irqrestore(&g_sched_lock, irqf);
    return target;
}

void proc_get_locked(process_t *p)
{
    if (p) __atomic_add_fetch(&p->hold_count, 1, __ATOMIC_SEQ_CST);
}

void proc_put(process_t *p)
{
    if (p) __atomic_sub_fetch(&p->hold_count, 1, __ATOMIC_SEQ_CST);
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
        for (thread_t *th = p->threads; th; th = th->proc_next) {
            if (th->tid == pid) {
                target = p;
                break;
            }
        }
        if (target) break;
    }

    if (!target) {
        spinlock_unlock_irqrestore(&g_sched_lock, irqf);
        return -(s64)ESRCH;
    }

    if (sig == 0) {
        spinlock_unlock_irqrestore(&g_sched_lock, irqf);
        return 0; /* Signal 0: check existence */
    }

    /* ── Job control ─────────────────────────────────────────────────────
     * Handled before the disposition lookup below, because a stop signal does
     * not terminate and SIGCONT has a side effect that runs whether or not a
     * handler is installed. Until this existed SIGSTOP fell all the way to the
     * fatal path and killed its target — which is why signal.c used to say
     * there was no stopped process state to enter. */
    if (sig == SIGCONT) {
        bool was_stopped = (target->stop_state != PROC_STOP_NONE);
        __atomic_store_n(&target->stop_state, PROC_STOP_NONE, __ATOMIC_RELEASE);
        target->stop_notified = false;
        /* POSIX: continuing discards every stop signal still pending. */
        __atomic_and_fetch(&target->sig_pending,
                           ~((1ULL << SIGSTOP) | (1ULL << SIGTSTP) |
                             (1ULL << 21 /*SIGTTIN*/) | (1ULL << 22 /*SIGTTOU*/)),
                           __ATOMIC_SEQ_CST);
        if (was_stopped) {
            target->cont_pending = true;
            for (thread_t *t = target->threads; t; t = t->proc_next) {
                if (!t->stopped) continue;
                if (t->state == THREAD_BLOCKED)              enqueue_ready(t);
                else if (t->state == THREAD_BLOCKED_PENDING) t->unblock_pending = true;
                else if (t->state == THREAD_RUNNING)         t->state = THREAD_READY;
            }
            notify_waiter_locked(target->parent, SIGCHLD);
        }
        /* The resume happens either way; the *disposition* still applies, so a
         * process with a SIGCONT handler falls through to have it queued. */
        sighandler_t ch = target->sigactions[SIGCONT].sa_handler;
        if (ch == SIG_DFL || ch == SIG_IGN) {
            spinlock_unlock_irqrestore(&g_sched_lock, irqf);
            return 0;
        }
    }

    if (sig == SIGSTOP || sig == SIGTSTP ||
        sig == 21 /*SIGTTIN*/ || sig == 22 /*SIGTTOU*/) {
        sighandler_t sh = target->sigactions[sig].sa_handler;
        /* SIGSTOP is unconditional; the other three stop only while their
         * disposition is still the default. */
        if (sig == SIGSTOP || sh == SIG_DFL) {
            /* When someone is tracing, the same stop belongs to the tracer:
             * it is the tracer that will be woken, and it is PTRACE_CONT — not
             * SIGCONT — that ends it. Recording which kind of stop this is
             * makes wait4() report it as a ptrace-stop and /proc show 't
             * (tracing stop)' instead of 'T (stopped)'. This is the path a
             * tracee's own raise(SIGSTOP) takes to hand over control. */
            u32 kind = PROC_STOP_JOB;
            if (target->tracer_pid) {
                kind = PROC_STOP_PTRACE;
                target->ptrace_stop_sig = sig;
                target->ptrace_event    = 0;
            }
            request_stop_locked(target, sig, kind);
            spinlock_unlock_irqrestore(&g_sched_lock, irqf);
            return 0;
        }
        if (sh == SIG_IGN) {
            spinlock_unlock_irqrestore(&g_sched_lock, irqf);
            return 0;
        }
        /* Custom handler: fall through and queue it like any other signal. */
    }

    /* BUG-S fix: SIGKILL and SIGSTOP cannot be caught, ignored, or blocked
     * per POSIX.  Exclude them BEFORE checking the handler so that a corrupted
     * sigactions table cannot make SIGKILL a no-op. */
    if (sig > 0 && sig < _NSIG && sig != SIGKILL && sig != SIGSTOP) {
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
            /*
             * POSIX: a blocked signal stays pending; its action is taken when
             * it is unblocked, or consumed by sigwait()/sigtimedwait().  Only
             * SIGKILL and SIGSTOP ignore the mask.  Without this a process
             * that blocked, say, SIGUSR2 would be killed by it anyway.
             */
            if (sig != 9 /* SIGKILL */ && sig != 19 /* SIGSTOP */ &&
                (target->sig_blocked & (1ULL << sig))) {
                __atomic_or_fetch(&target->sig_pending, (1ULL << sig), __ATOMIC_SEQ_CST);
                spinlock_unlock_irqrestore(&g_sched_lock, irqf);
                return 0;
            }
        } else {
            /* Custom handler registered */
            __atomic_or_fetch(&target->sig_pending, (1ULL << sig), __ATOMIC_SEQ_CST);
            /* Wake up blocked/sleeping threads to handle signal */
            for (thread_t *t = target->threads; t; t = t->proc_next) {
                if (t->state == THREAD_BLOCKED || t->state == THREAD_SLEEPING) {
                    if (t->state == THREAD_SLEEPING) sleep_queue_remove_locked(t);
                    enqueue_ready(t);
                } else if (t->state == THREAD_BLOCKED_PENDING ||
                           t->state == THREAD_SLEEPING_PENDING) {
                    /* BUG-6: thread is mid-switch (between sched_block and
                     * sched_post_switch). Setting unblock_pending causes
                     * sched_post_switch to enqueue_ready() it instead of
                     * blocking, so the signal is never silently lost. */
                    t->unblock_pending = true;
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

    /* A stopped process has to be let out of the stop before it can die. Its
     * threads are parked in THREAD_BLOCKED inside sched_stop_current(), and a
     * blocked thread marked THREAD_DYING is never scheduled again, so it would
     * never reach sched_post_switch() to be zombified. Clearing stop_state and
     * making those threads runnable lets each one fall out of the stop loop and
     * take the exit in its own context — see sched_stop_current().
     *
     * The test is thread_t::stopped, not the process's stop_state: a SIGCONT
     * that arrived moments earlier clears stop_state while its threads are
     * still parked, and gating on the process flag would leave exactly those
     * threads marked DYING and blocked forever. */
    __atomic_store_n(&target->stop_state, PROC_STOP_NONE, __ATOMIC_RELEASE);

    for (thread_t *t = target->threads; t; t = t->proc_next) {
        if (t->stopped) {
            if (t->state == THREAD_BLOCKED)              enqueue_ready(t);
            else if (t->state == THREAD_BLOCKED_PENDING) t->unblock_pending = true;
            else if (t->state == THREAD_RUNNING)         t->state = THREAD_READY;
            continue;
        }
        if (t->state == THREAD_BLOCKED || t->state == THREAD_SLEEPING) {
            /* Remove from sleep queue if sleeping */
            if (t->state == THREAD_SLEEPING) sleep_queue_remove_locked(t);
            /* Thread is descheduled and off-CPU; promote straight to ZOMBIE */
            t->state = THREAD_ZOMBIE;
        } else if (t->state == THREAD_READY) {
            /* Remove from ready queue */
            for (thread_t **pp = &g_ready_queue; *pp; pp = &(*pp)->next) {
                if (*pp == t) {
                    *pp = t->next;
                    break;
                }
            }
            t->state = THREAD_ZOMBIE;
        } else if (t->state == THREAD_BLOCKED_PENDING || t->state == THREAD_SLEEPING_PENDING) {
            /* Mid-switch on a CPU: mark DYING so sched_post_switch zombifies it */
            t->state = THREAD_DYING;
        } else if (t->state == THREAD_RUNNING) {
            /* Running on a CPU: mark DYING; if on another CPU, poke it */
            t->state = THREAD_DYING;
            if (t->cpu_id != smp_current_cpu_id()) {
                smp_send_reschedule(t->cpu_id);
            }
        }
        /* THREAD_DYING / THREAD_ZOMBIE: already on the way out, leave as-is */
    }

    bool all_dead = true;
    for (thread_t *t = target->threads; t; t = t->proc_next) {
        if (t->state != THREAD_ZOMBIE) {
            all_dead = false;
            break;
        }
    }
    if (all_dead) {
        target->is_zombie = true;
        notify_waiter_locked(target->parent, SIGCHLD);
        if (target->tracer_pid && (!target->parent || target->parent->pid != target->tracer_pid)) {
            for (process_t *tr = g_process_list; tr; tr = tr->next) {
                if (tr->pid == target->tracer_pid) { notify_waiter_locked(tr, SIGCHLD); break; }
            }
        }
    }

    bool self_killed = (target == sched_current_process());
    spinlock_unlock_irqrestore(&g_sched_lock, irqf);

    if (self_killed) {
        sched_exit_thread();
        __builtin_unreachable();
    }
    return 0;
}

process_t *sched_kernel_process(void)
{
    return g_kernel_proc;
}

process_t *sched_get_process_list(void)
{
    return g_process_list;
}

bool sched_proc_ident(u32 pid, struct proc_ident *out)
{
    bool found = false;
    irqflags_t irqf = spinlock_lock_irqsave(&g_sched_lock);
    for (process_t *p = g_process_list; p; p = p->next) {
        if (p->pid != pid) continue;
        out->pid       = p->pid;
        out->ppid      = p->parent ? p->parent->pid : 0;
        out->uid       = p->uid;
        out->gid       = p->gid;
        out->euid      = p->euid;
        out->egid      = p->egid;
        out->suid      = p->suid;
        out->sgid      = p->sgid;
        out->pml4_phys = p->pml4_phys;
        out->is_zombie = p->is_zombie;
        found = true;
        break;
    }
    spinlock_unlock_irqrestore(&g_sched_lock, irqf);
    return found;
}

static irqflags_t g_sched_proc_irqf[SMP_MAX_CPUS];

void sched_lock(void)
{
    u32 cpu_id = smp_current_cpu_id();
    if (cpu_id >= SMP_MAX_CPUS) cpu_id = 0;
    /* BUG-V: sched_lock is NOT reentrant.  A second call on the same CPU
     * overwrites g_sched_proc_irqf[cpu_id] (losing the outer IRQ save) and
     * deadlocks on the ticket lock.  Catch this immediately in debug builds. */
    BUG_ON(g_sched_proc_irqf[cpu_id] & (1UL << 63)); /* sentinel: top bit set = locked */
    g_sched_proc_irqf[cpu_id] = spinlock_lock_irqsave(&g_sched_lock);
    /* Mark slot in-use so a nested call trips the BUG_ON above. */
    g_sched_proc_irqf[cpu_id] |= (1UL << 63);
}

void sched_unlock(void)
{
    u32 cpu_id = smp_current_cpu_id();
    if (cpu_id >= SMP_MAX_CPUS) cpu_id = 0;
    /* Clear the in-use sentinel bit before restoring flags. */
    irqflags_t flags = g_sched_proc_irqf[cpu_id] & ~(1UL << 63);
    g_sched_proc_irqf[cpu_id] = 0;
    spinlock_unlock_irqrestore(&g_sched_lock, flags);
}

u64 sched_get_ticks(void)
{
    return __atomic_load_n(&g_system_ticks, __ATOMIC_RELAXED);
}

