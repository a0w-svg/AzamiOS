/* ============================================================================
 * AzamiOS — CFS Scheduler & Process/Thread Management Implementation
 * File: kernel/sched/sched.c
 * ============================================================================ */

#define DEBUG 0
#include <azami/debug.h>
#include "sched.h"
#include "rq_tree.h"
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
#include "../../arch/x86_64/cpu/topology.h"
#include "../../arch/x86_64/cpu/lapic.h"
#include "../../arch/x86_64/cpu/spinlock.h"
#include "../../arch/x86_64/cpu/msr.h"
#include "../../arch/x86_64/cpu/cpu.h"
#include "../../drivers/char/console.h"
#include "../../include/azami/defs.h"
#include "../../fs/vfs.h"
#include "../../arch/x86_64/cpu/hwaccel.h"
#include "../time/timekeeping.h"


static spinlock_t g_sched_lock = SPINLOCK_INIT;
static process_t *g_process_list = NULL;
static process_t *g_kernel_proc = NULL;
static u32 g_next_pid = 1;
static u32 g_next_pcid = 0;   /* rolls 1..4095 for user address spaces */
static u32 g_next_tid = 1;

/* Per-CPU CFS Runqueues (SCHED-SMP-01)
 *
 * Each CPU manages its own runqueue guarded by its own spinlock. When a CPU's
 * local queue is depleted, it performs work-stealing from the busiest core's
 * queue for any threads allowing that CPU in their affinity_mask.
 *
 * Locking hierarchy: g_sched_lock -> rq->lock. Never acquire g_sched_lock
 * while holding any rq->lock. */
typedef struct runqueue {
    spinlock_t lock;
    rq_tree_t  tree;          /* ready CFS threads, ordered by (vruntime, tid) */
    u64        min_vruntime;
    u32        nr_running;    /* CFS + RT threads queued here                  */

    /* ── Real-time run queue (SCHED_FIFO / SCHED_RR) ──────────────────────
     *
     * A single list per CPU, kept in descending rt_priority order and FIFO
     * within a priority — which is exactly what POSIX specifies for both
     * real-time policies. Linux uses an array of 100 per-priority lists plus
     * a bitmap so the highest runnable priority is a find-first-set; that is
     * the right shape when hundreds of real-time tasks are expected, and the
     * wrong one here, where it would cost 100 KB of per-CPU list heads across
     * SMP_MAX_CPUS to make an already-short list marginally faster to index.
     * Insertion walks the list, which is O(number of RT threads on this CPU)
     * — and the whole point of a real-time policy is that there are few of
     * them. Picking the next one to run is O(1) either way: it is the head.
     *
     * Every thread on this list outranks every thread in the tree above. That
     * is the defining property of the real-time classes and the reason they
     * cannot be expressed as an extreme CFS weight, which is what this
     * scheduler did before: a SCHED_FIFO thread mapped to weight 1 still
     * yielded to a nice-19 thread as soon as its vruntime crept ahead. */
    thread_t  *rt_head;
    u32        rt_nr_running;

    /* Staggered so neighbouring CPUs do not all try to balance on the same
     * tick and pile onto each other's run-queue locks. */
    u64        next_balance_tick;
} runqueue_t;

/* ── CFS load weights ─────────────────────────────────────────────────────
 *
 * Linux's sched_prio_to_weight[], indexed by nice + 20. The table is
 * geometric with a ratio of about 1.25 per nice level, chosen so that one
 * nice step changes a task's CPU share by roughly 10% regardless of where in
 * the range it sits, and so that the relationship holds for any number of
 * competing tasks.
 *
 * This replaces a linear "10 + nice, clamped to [1,39]" mapping used as a
 * divisor. That mapping gave nice -20 only 29 times the share of nice +19,
 * where Linux gives it 5917 times — so renicing barely did anything, and
 * `nice -n 19` on a runaway process still let it take a third of the CPU
 * against a single nice-0 competitor. Matching the real table is what makes
 * nice(2) and setpriority(2) mean on this kernel what they mean everywhere
 * else, which is the whole point of implementing them.
 */
#define NICE_0_WEIGHT     1024u
#define SCHED_VTIME_UNIT  1024u    /* vruntime granularity per tick at nice 0 */

static const u32 g_nice_to_weight[40] = {
    /* -20 */ 88761, 71755, 56483, 46273, 36291,
    /* -15 */ 29154, 23254, 18705, 14949, 11916,
    /* -10 */  9548,  7620,  6100,  4904,  3906,
    /*  -5 */  3121,  2501,  1991,  1586,  1277,
    /*   0 */  1024,   820,   655,   526,   423,
    /*   5 */   335,   272,   215,   172,   137,
    /*  10 */   110,    87,    70,    56,    45,
    /*  15 */    36,    29,    23,    18,    15,
};

/* SCHED_IDLE runs only when nothing else wants the CPU. Linux uses weight 3
 * (WEIGHT_IDLEPRIO); anything smaller starves it outright rather than merely
 * deprioritising it, and POSIX still requires it to make progress. */
#define WEIGHT_IDLEPRIO   3u

/* SCHED_RR time slice, in scheduler ticks. Linux's default is 100 ms; at the
 * 100 Hz tick this kernel runs, that is 10 ticks. sched_rr_get_interval(2)
 * reports it. */
#define SCHED_RR_TICKS    10u

/* How often a CPU looks for work to pull when it is *not* idle. The idle path
 * steals immediately (see dequeue_ready), which covers the common case; this
 * covers the one it cannot — every CPU busy, but one of them holding several
 * runnable threads while another holds one. At 100 Hz this is every 40 ms,
 * close to Linux's default balance interval for the innermost domain. */
#define SCHED_BALANCE_INTERVAL_TICKS  4u

/* The rate kernel_main() and every AP start the LAPIC timer at. Named here
 * because the scheduler's own conversions between ticks and real time depend
 * on it, and a literal 100 repeated across three files is a mismatch waiting
 * to happen the first time the tick rate changes. */
#define SCHED_TICK_HZ  100u

/* The minimum-vruntime thread, i.e. whoever runs next on this CPU. Takes over
 * the role rq->head played for the lock-free readers below ("is anything
 * runnable here?" and the address the idle loop arms MONITOR on): written
 * under rq->lock, read without it. */
#define rq_head(rq) ((rq)->tree.leftmost)

static runqueue_t g_cpu_rq[SMP_MAX_CPUS];

static void enqueue_ready(thread_t *t);
static void rq_remove_thread_locked(thread_t *t);

/* Sleep queue is kept sorted by sleep_end_ticks (ascending) for O(1) tick scan */
static thread_t *g_sleep_queue = NULL;
u64 g_system_ticks = 0;

static thread_t *g_idle_threads[SMP_MAX_CPUS] = {NULL};

/* Real context-switch count for /proc/stat's "ctxt" line and vmstat's "cs"
 * column -- both used to be fixed constants (250 in vmstat.elf) that never
 * moved. sched_post_switch() is the one chokepoint every switch_to_asm()
 * call site (there are 4) always lands in on the far side, so counting
 * there catches all of them without touching each call site individually. */
u64 g_context_switches = 0;

/* Per-CPU idle bitmask: bit i is set when CPU i is running its idle thread.
 * Allows enqueue_ready() to find an idle CPU in O(1) via __builtin_ctzll(). */
static volatile u64 g_idle_cpu_mask = 0;

static void sleep_queue_insert_sorted(thread_t *t); /* forward decl */
static void notify_waiter_locked(process_t *parent, int sig); /* forward decl */

/* ── Type-stable thread_t allocation ───────────────────────────────────────
 *
 * A retired thread_t is recycled through this private free list and never
 * handed back to kmalloc(), so the memory a thread_t once occupied stays a
 * thread_t for the life of the system.
 *
 * That is what makes the kernel's stale wait-slot pointers survivable. Sockets,
 * pipes, IPC channels and semaphores each remember the single thread parked on
 * them as a bare thread_t* (sock->rx_wait_thread, pipe readers, ipc senders,
 * and ~30 more slots across net/ and fs/), and the parked thread is the only
 * thing that ever clears its own slot — on the far side of a wake-up it may
 * never get, because a thread killed while blocked is zombified in place. The
 * pointer then outlives the thread, and the object's teardown wakes through it.
 *
 * With the memory permanently typed, that wake-up reads a well-formed thread_t
 * instead of whatever kmalloc() handed the address to next, and the magic
 * stamp tells it apart from a live one: sched_unblock() and enqueue_ready()
 * both reject a retired thread outright. The residual case — the struct
 * already recycled for a new thread — wakes the wrong thread instead of
 * scheduling freed memory, and every blocking site in this kernel re-tests its
 * condition in a loop after sched_block() returns, so a spurious wake-up is
 * absorbed rather than acted on.
 *
 * The list is bounded by the system's peak concurrent thread count (a thread_t
 * lands in kmalloc's 4 KB bucket), and those structs are exactly what the next
 * burst of threads needs, so nothing is gained by ever releasing them. Keeping
 * them also takes thread creation and exit off the shared allocator entirely.
 */
static spinlock_t g_thread_cache_lock = SPINLOCK_INIT;
static thread_t  *g_thread_cache;        /* linked through proc_next */
static u32        g_thread_cache_count;

static thread_t *thread_alloc(void)
{
    irqflags_t f = spinlock_lock_irqsave(&g_thread_cache_lock);
    thread_t *t = g_thread_cache;
    if (t) {
        g_thread_cache = t->proc_next;
        g_thread_cache_count--;
    }
    spinlock_unlock_irqrestore(&g_thread_cache_lock, f);

    if (t) __builtin_memset(t, 0, sizeof(*t));
    else   t = (thread_t *)kzalloc(sizeof(thread_t));

    if (t) t->magic = THREAD_MAGIC;
    return t;
}

static void thread_free(thread_t *t)
{
    if (!t) return;
    if (t->magic != THREAD_MAGIC) {
        /* Replaces the double-free check kfree() used to provide for these
         * structs, and catches a wild pointer reaching this path at all. */
        PANIC("thread_free on a non-thread or already-retired thread_t! "
              "t=%p magic=0x%x (expected 0x%x) caller=%p",
              (void *)t, (unsigned int)t->magic, (unsigned int)THREAD_MAGIC,
              __builtin_return_address(0));
    }
    t->magic = 0;

    irqflags_t f = spinlock_lock_irqsave(&g_thread_cache_lock);
    t->proc_next = g_thread_cache;
    g_thread_cache = t;
    g_thread_cache_count++;
    spinlock_unlock_irqrestore(&g_thread_cache_lock, f);
}

/*
 * Retire @t for good. Caller holds g_sched_lock.
 *
 * Setting THREAD_ZOMBIE by hand is not enough, because process_t::wait_thread
 * is a bare back-pointer to whichever thread parked itself in sched_waitpid(),
 * and only that thread ever cleared it — on the far side of its own wake-up.
 * A thread killed while parked there (SIGKILL, or a sibling's exit_group)
 * never reaches that code, so the pointer outlived the thread: the reaper
 * freed the thread_t, and the next child exit walked straight into it through
 * notify_waiter_locked(), read its state out of freed memory and linked it
 * into a run queue to be scheduled. Clearing the back-pointer at the one
 * moment the thread dies closes that off wherever the kill comes from.
 */
static inline void thread_zombify_locked(thread_t *t)
{
    t->state = THREAD_ZOMBIE;
    if (t->proc && t->proc->wait_thread == t)
        t->proc->wait_thread = NULL;
}

void sched_post_switch(void)
{
    cpu_info_t *cpu = smp_get_cpu();
    if (!cpu) return;

    __atomic_add_fetch(&g_context_switches, 1, __ATOMIC_RELAXED);

    /* Precise CPU-time accounting: charge the run that just ended, start
     * the clock on the one beginning. Both threads are pinned to this CPU
     * for the duration (prev is not re-enqueued until further down), so
     * only the process total needs an atomic. */
    u64 now_ns = ktime_get_ns();
    thread_t *outgoing = cpu->prev_thread;
    if (outgoing && outgoing->exec_start_ns) {
        u64 ran = now_ns - outgoing->exec_start_ns;
        outgoing->sum_exec_ns += ran;
        outgoing->exec_start_ns = 0;
        if (outgoing->proc)
            __atomic_add_fetch(&outgoing->proc->sum_exec_ns, ran, __ATOMIC_RELAXED);
    }
    if (cpu->current_thread)
        cpu->current_thread->exec_start_ns = now_ns ? now_ns : 1;

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
            thread_zombify_locked(prev);
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

/* ── Real-time run queue ──────────────────────────────────────────────────
 *
 * Descending rt_priority, FIFO within a priority. Caller holds rq->lock. */

static void rt_enqueue_locked(runqueue_t *rq, thread_t *t, bool head)
{
    thread_t **link = &rq->rt_head;

    /* `head` requeues a preempted thread ahead of its equals, which is what
     * SCHED_FIFO requires: a FIFO thread that was preempted by a
     * higher-priority one resumes at the *front* of its priority, not behind
     * peers that were waiting. A thread that used up an RR slice goes to the
     * back instead, which is what makes RR round-robin at all. */
    while (*link) {
        if (head ? ((*link)->rt_priority < t->rt_priority)
                 : ((*link)->rt_priority <= t->rt_priority))
            break;
        link = &(*link)->rt_next;
    }

    t->rt_next = *link;
    *link = t;
    rq->rt_nr_running++;
    rq->nr_running++;
}

static bool rt_erase_locked(runqueue_t *rq, thread_t *t)
{
    for (thread_t **link = &rq->rt_head; *link; link = &(*link)->rt_next) {
        if (*link == t) {
            *link = t->rt_next;
            t->rt_next = NULL;
            if (rq->rt_nr_running > 0) rq->rt_nr_running--;
            if (rq->nr_running > 0) rq->nr_running--;
            return true;
        }
    }
    return false;
}

static thread_t *rt_pick_locked(runqueue_t *rq)
{
    thread_t *t = rq->rt_head;
    if (!t) return NULL;
    rq->rt_head = t->rt_next;
    t->rt_next = NULL;
    if (rq->rt_nr_running > 0) rq->rt_nr_running--;
    if (rq->nr_running > 0) rq->nr_running--;
    return t;
}

/* Unlink @t from run queue @cpu if it is there. Caller holds no rq lock.
 * Returns true when the thread was found and removed. */
static bool rq_try_remove_on(u32 cpu, thread_t *t)
{
    runqueue_t *rq = &g_cpu_rq[cpu];
    spinlock_lock(&rq->lock);
    /* rq_cpu says which queue @t is linked on, so this is a membership test
     * and an O(log n) erase rather than a walk of the whole queue. */
    if (__atomic_load_n(&t->rq_cpu, __ATOMIC_RELAXED) == cpu) {
        if (t->rt_priority) {
            rt_erase_locked(rq, t);
        } else {
            rq_tree_erase(&rq->tree, t);
            if (rq->nr_running > 0) rq->nr_running--;
        }
        t->rq_cpu = (u32)-1;
        spinlock_unlock(&rq->lock);
        return true;
    }
    spinlock_unlock(&rq->lock);
    return false;
}

static void rq_remove_thread_locked(thread_t *t)
{
    u32 ncpus = smp_cpu_count();
    if (ncpus == 0) ncpus = 1;
    if (ncpus > SMP_MAX_CPUS) ncpus = SMP_MAX_CPUS;

    /* enqueue_ready() records which queue it linked the thread onto, so the
     * usual case is one lock acquisition instead of one per possible CPU.
     * This used to walk all SMP_MAX_CPUS (64) queues, locking each, even on a
     * 4-core machine where 60 of them are permanently empty — and it runs once
     * per READY thread in sched_exit_group_mark() and sched_kill_process(),
     * i.e. once per thread of a dying multi-threaded process. */
    u32 hint = t->rq_cpu;
    if (hint < ncpus && rq_try_remove_on(hint, t)) return;

    /* The hint was read without a lock, so the thread may have migrated
     * between that read and rq_try_remove_on() taking the queue's lock. Retry
     * the other online queues; each re-reads rq_cpu under its own lock, so
     * whichever one currently owns the thread will claim it. */
    for (u32 i = 0; i < ncpus; i++) {
        if (i == hint) continue;
        if (rq_try_remove_on(i, t)) return;
    }
    t->rq_cpu = (u32)-1;
}

/*
 * Pick the CPU a waking thread should run on.
 *
 * Preference order, strongest first:
 *
 *   1. The CPU it last ran on, if that CPU is idle. Its cache is still warm
 *      and nothing has to be moved.
 *   2. An idle CPU on a physical core where *no* sibling thread is busy. On
 *      an SMT machine two logical CPUs share one core's execution resources,
 *      so putting the second thread on an already-occupied core buys far less
 *      than a quarter of a core's throughput — filling distinct cores first
 *      is worth more than any cache locality it gives up. This is the single
 *      biggest thing plain "first idle bit" placement gets wrong on
 *      hyperthreaded hardware.
 *   3. An idle CPU sharing a last-level cache with where it last ran, so a
 *      migration at least keeps the L3 working set.
 *   4. Any idle CPU at all.
 *   5. Failing all that, the least loaded CPU it is allowed on.
 */
static u32 sched_select_cpu(thread_t *t, u64 valid_cpus, u64 idle_mask, u32 my_cpu, u32 ncpus)
{
    /* 1. Cache warmth: stay put when the previous CPU is idle and permitted. */
    if (t->cpu_id < ncpus && (valid_cpus & (1ULL << t->cpu_id)) &&
        (idle_mask & (1ULL << t->cpu_id)))
        return t->cpu_id;

    u64 idle_candidates = idle_mask & valid_cpus;

    if (idle_candidates) {
        /* 2. An idle CPU whose SMT siblings are also idle — i.e. a fully
         *    unused physical core. */
        u64 scan = idle_candidates;
        while (scan) {
            u32 c = hw_ctz64(scan);
            scan &= scan - 1;
            u64 sibs = topology_smt_siblings(c);
            /* No topology (or no siblings) means the CPU *is* a whole core,
             * which satisfies the preference trivially. */
            if (!sibs || (sibs & ~idle_mask) == 0)
                return c;
        }

        /* 3. An idle CPU sharing a last-level cache with the previous one. */
        if (t->cpu_id < ncpus) {
            u64 llc = topology_core_siblings(t->cpu_id) & idle_candidates;
            if (llc) return hw_ctz64(llc);
        }

        /* 4. Any idle CPU. */
        return hw_ctz64(idle_candidates);
    }

    /* 5. Least loaded permitted CPU. The caller's own CPU is seeded first so
     *    that a tie keeps the thread local rather than bouncing it. */
    u32 target_cpu = (u32)-1;
    u32 min_load = (u32)-1;
    if (valid_cpus & (1ULL << my_cpu)) {
        target_cpu = my_cpu;
        min_load = __atomic_load_n(&g_cpu_rq[my_cpu].nr_running, __ATOMIC_RELAXED);
    }
    for (u32 i = 0; i < ncpus; i++) {
        if (!(valid_cpus & (1ULL << i))) continue;
        u32 nr = __atomic_load_n(&g_cpu_rq[i].nr_running, __ATOMIC_RELAXED);
        if (nr < min_load) {
            min_load = nr;
            target_cpu = i;
        }
    }
    return target_cpu;
}

static void enqueue_ready(thread_t *t)
{
    /* A ZOMBIE thread is off every CPU for good and its thread_t is on its way
     * to kfree() (or already through it). Linking one into a run queue means
     * scheduling freed memory, so refuse here rather than trusting every
     * wake-up path to have checked. The stale-waiter paths that used to be
     * able to do this — proc->wait_thread left dangling by a thread killed
     * while parked in waitpid — are fixed at the source too, but this is the
     * backstop that makes the invariant hold for future wake-up sites. */
    if (t->state == THREAD_ZOMBIE) return;

    /* Retired struct reached through a stale wait slot — see thread_alloc(). */
    if (t->magic != THREAD_MAGIC) return;

    /* Already linked on some CPU's run-queue tree. Re-inserting a node that
     * is still in a tree would corrupt it, and the thread is going to be
     * scheduled from where it already sits anyway. rq_cpu is the authority
     * on this: every insertion sets it and every removal clears it, both
     * under the owning queue's lock, which is also what lets
     * rq_try_remove_on() unlink in O(log n) without searching. */
    if (__atomic_load_n(&t->rq_cpu, __ATOMIC_RELAXED) != (u32)-1) {
        t->state = THREAD_READY;
        return;
    }

    t->state = THREAD_READY;
    t->next = NULL;
    t->rt_next = NULL;

    u32 ncpus = smp_cpu_count();
    if (ncpus == 0) ncpus = 1;
    if (ncpus > SMP_MAX_CPUS) ncpus = SMP_MAX_CPUS;

    u64 online_mask = (ncpus >= 64) ? ~0ULL : ((1ULL << ncpus) - 1);

    /* A CPU that never finished bringing itself up cannot run anything and
     * will not answer a wakeup IPI, so it must not be a placement target —
     * a thread queued there would simply never run again. */
    u64 up = smp_online_mask();
    if (up) online_mask &= up;
    if (!online_mask) online_mask = 1ULL;   /* BSP, pre-SMP */

    u64 valid_cpus = t->affinity_mask & online_mask;
    if (valid_cpus == 0) valid_cpus = online_mask;

    u64 idle_mask = __atomic_load_n(&g_idle_cpu_mask, __ATOMIC_RELAXED) & online_mask;
    u32 my_cpu = smp_current_cpu_id();
    if (my_cpu >= SMP_MAX_CPUS) my_cpu = 0;

    u32 target_cpu = sched_select_cpu(t, valid_cpus, idle_mask, my_cpu, ncpus);
    if (target_cpu >= ncpus || !(valid_cpus & (1ULL << target_cpu)))
        target_cpu = hw_ctz64(valid_cpus);
    if (target_cpu >= ncpus) target_cpu = 0;

    runqueue_t *rq = &g_cpu_rq[target_cpu];
    bool preempt_target = false;

    spinlock_lock(&rq->lock);

    if (t->rt_priority) {
        /* Real time: no vruntime, no tree. A thread that still has slice
         * left goes in ahead of its equals — it was waiting on an event, or
         * was preempted by something higher, and POSIX says it resumes at the
         * front of its priority. A SCHED_RR thread whose slice sched_tick()
         * ran down to zero goes to the back instead, which is the whole of
         * what makes RR round-robin. */
        rt_enqueue_locked(rq, t, t->rr_ticks_left != 0);
        preempt_target = true;   /* always outranks whatever CFS is running */
    } else {
        /* Clamp up to the run-queue floor so a stale-low vruntime cannot starve the queue */
        if (t->vruntime < rq->min_vruntime) t->vruntime = rq->min_vruntime;

        /* O(log n), and the same cost wherever the thread sorts. The list this
         * replaced special-cased head and tail insertion but walked the queue for
         * anything in between — which is exactly where a waking thread lands. */
        rq_tree_insert(&rq->tree, t);
        rq->nr_running++;
    }
    t->rq_cpu = target_cpu;
    spinlock_unlock(&rq->lock);

    /* Wake up target CPU if it was idle, or request local reschedule */
    if (target_cpu != my_cpu) {
        if (preempt_target || (idle_mask & (1ULL << target_cpu))) {
            smp_send_reschedule(target_cpu);
        }
    } else {
        cpu_info_t *cpu = smp_get_cpu();
        thread_t *cur = cpu ? cpu->current_thread : NULL;
        if (cpu && (preempt_target ||
                    cur == g_idle_threads[my_cpu] ||
                    (rq_head(rq) == t && cur && !cur->rt_priority &&
                     t->vruntime < cur->vruntime))) {
            cpu->needs_reschedule = true;
        }
    }
}

/*
 * Try to take one runnable thread off @victim that is allowed to run on
 * @thief. Returns it, unlinked from the victim's queue, or NULL.
 *
 * Real-time threads are considered first and unconditionally: a FIFO/RR
 * thread sitting queued while another CPU idles is the one case this
 * scheduler must never allow, since its whole contract is that it runs as
 * soon as a CPU is available.
 */
static thread_t *rq_steal_from(u32 victim, u32 thief, u64 thief_floor)
{
    runqueue_t *vrq = &g_cpu_rq[victim];

    spinlock_lock(&vrq->lock);

    for (thread_t *rt = vrq->rt_head; rt; rt = rt->rt_next) {
        if (rt->affinity_mask & (1ULL << thief)) {
            rt_erase_locked(vrq, rt);
            rt->rq_cpu = (u32)-1;
            spinlock_unlock(&vrq->lock);
            return rt;
        }
    }

    /* In-order walk: the first thread that may run here is also the one with
     * the smallest vruntime, the same thread the list walk would have found.
     * Almost always the very first node, since affinity masks are usually
     * "any CPU". */
    thread_t *curr = rq_tree_first(&vrq->tree);
    while (curr) {
        thread_t *nxt = rq_tree_next(curr);
        if (curr->affinity_mask & (1ULL << thief)) {
            rq_tree_erase(&vrq->tree, curr);
            curr->rq_cpu = (u32)-1;
            if (vrq->nr_running > 0) vrq->nr_running--;
            if (curr->vruntime < thief_floor)
                curr->vruntime = thief_floor;
            spinlock_unlock(&vrq->lock);
            return curr;
        }
        curr = nxt;
    }

    spinlock_unlock(&vrq->lock);
    return NULL;
}

/*
 * Find the CPU worth stealing from.
 *
 * "Busiest" alone is not the right answer on a machine with more than one
 * cache domain: pulling a thread from a CPU that shares this one's last-level
 * cache keeps its working set resident, while pulling across a package
 * boundary means every line it touches has to be fetched again. So the search
 * runs twice — once restricted to the local cache domain, and only if that
 * finds nothing, across the whole machine. This is the same nested-domain
 * shape Linux's sched_domain hierarchy gives load balancing, expressed
 * directly rather than through a generic domain tree.
 *
 * @min_nr is the load a candidate must exceed to be worth taking from: 0 when
 * the thief is idle (anything beats running nothing), and the thief's own
 * load + 1 when it is merely balancing, so a steal cannot reverse the
 * imbalance it is correcting.
 */
static u32 rq_find_busiest(u32 thief, u32 ncpus, u32 min_nr, bool same_llc_only)
{
    u64 llc = topology_core_siblings(thief);
    u32 best = (u32)-1;
    u32 best_nr = min_nr;

    for (u32 i = 0; i < ncpus; i++) {
        if (i == thief) continue;
        if (same_llc_only && llc && !(llc & (1ULL << i))) continue;

        u32 nr = __atomic_load_n(&g_cpu_rq[i].nr_running, __ATOMIC_RELAXED);
        if (nr > best_nr) {
            best_nr = nr;
            best = i;
        }
    }
    return best;
}

static thread_t *dequeue_ready(void)
{
    u32 my_cpu = smp_current_cpu_id();
    if (my_cpu >= SMP_MAX_CPUS) my_cpu = 0;

    runqueue_t *my_rq = &g_cpu_rq[my_cpu];
    spinlock_lock(&my_rq->lock);

    /* Real time first, always. Nothing in the CFS tree may run while a
     * SCHED_FIFO or SCHED_RR thread is runnable on this CPU. */
    thread_t *rt = rt_pick_locked(my_rq);
    if (rt) {
        rt->rq_cpu = (u32)-1;
        if (rt->rr_ticks_left == 0) rt->rr_ticks_left = SCHED_RR_TICKS;
        spinlock_unlock(&my_rq->lock);
        return rt;
    }

    thread_t *first = rq_tree_first(&my_rq->tree);
    if (first) {
        thread_t *t = first;
        rq_tree_erase(&my_rq->tree, t);
        t->rq_cpu = (u32)-1;
        if (my_rq->nr_running > 0) my_rq->nr_running--;
        if (t->vruntime > my_rq->min_vruntime) my_rq->min_vruntime = t->vruntime;
        spinlock_unlock(&my_rq->lock);
        return t;
    }
    spinlock_unlock(&my_rq->lock);

    /* Local queue is empty: perform work-stealing. */
    u32 ncpus = smp_cpu_count();
    if (ncpus > SMP_MAX_CPUS) ncpus = SMP_MAX_CPUS;
    if (ncpus <= 1) return NULL;

    /* Read our own floor *before* taking a victim's lock. Grabbing
     * my_rq->lock while holding vrq->lock would invert the order two CPUs
     * stealing from each other use, and deadlock; a slightly stale floor only
     * costs the stolen thread a little vruntime. */
    u64 my_floor = __atomic_load_n(&my_rq->min_vruntime, __ATOMIC_RELAXED);

    for (int pass = 0; pass < 2; pass++) {
        bool local_domain = (pass == 0);

        u32 best_victim = rq_find_busiest(my_cpu, ncpus, 0, local_domain);
        if (best_victim == (u32)-1) continue;

        /* Start at the busiest and fan outwards: by the time the lock is
         * taken the busiest may have emptied, and a second candidate is
         * cheaper to try than a fresh scan. */
        for (u32 attempt = 0; attempt < ncpus; attempt++) {
            u32 victim = (best_victim + attempt) % ncpus;
            if (victim == my_cpu) continue;
            if (__atomic_load_n(&g_cpu_rq[victim].nr_running, __ATOMIC_RELAXED) == 0) continue;

            thread_t *stolen = rq_steal_from(victim, my_cpu, my_floor);
            if (stolen) {
                if (stolen->proc) stolen->proc->nr_migrations++;
                return stolen;
            }
        }
    }

    return NULL;
}

/*
 * Periodic load balancing.
 *
 * dequeue_ready()'s steal covers the case where a CPU has run out of work
 * entirely. It cannot cover the case where every CPU has *something* to run
 * but the distribution is lopsided — four threads on one core and one each on
 * the other three — because no CPU there ever reaches the idle path. Without
 * this, that imbalance persists until a thread happens to block: the four
 * threads keep sharing one core at 25% each while three cores run at 100%.
 *
 * Run from sched_tick() on every CPU, staggered so neighbours do not contend,
 * and only when the imbalance is at least two threads — pulling on a
 * difference of one just moves the imbalance to the other CPU.
 */
static void sched_balance_tick(u32 my_cpu, u64 now)
{
    u32 ncpus = smp_cpu_count();
    if (ncpus <= 1 || ncpus > SMP_MAX_CPUS) return;

    runqueue_t *my_rq = &g_cpu_rq[my_cpu];
    if (now < my_rq->next_balance_tick) return;
    my_rq->next_balance_tick = now + SCHED_BALANCE_INTERVAL_TICKS;

    u32 my_nr = __atomic_load_n(&my_rq->nr_running, __ATOMIC_RELAXED);

    /* A victim must have at least two more queued threads than we do, so that
     * after the pull it still has at least as many as we end up with. */
    u32 victim = rq_find_busiest(my_cpu, ncpus, my_nr + 1, true);
    if (victim == (u32)-1)
        victim = rq_find_busiest(my_cpu, ncpus, my_nr + 1, false);
    if (victim == (u32)-1) return;

    u64 my_floor = __atomic_load_n(&my_rq->min_vruntime, __ATOMIC_RELAXED);
    thread_t *pulled = rq_steal_from(victim, my_cpu, my_floor);
    if (!pulled) return;

    if (pulled->proc) pulled->proc->nr_migrations++;
    pulled->cpu_id = my_cpu;

    /* Put it on our own queue rather than running it here: this is a timer
     * interrupt, and the decision of what runs next belongs to the reschedule
     * that follows it. */
    spinlock_lock(&my_rq->lock);
    if (pulled->rt_priority) {
        rt_enqueue_locked(my_rq, pulled, true);
    } else {
        if (pulled->vruntime < my_rq->min_vruntime) pulled->vruntime = my_rq->min_vruntime;
        rq_tree_insert(&my_rq->tree, pulled);
        my_rq->nr_running++;
    }
    pulled->rq_cpu = my_cpu;
    spinlock_unlock(&my_rq->lock);
}

u64 sched_rr_interval_ns(void)
{
    /* SCHED_RR_TICKS slices at the scheduler's own tick rate. Reporting a
     * fixed 10 ms (what this used to answer) was wrong twice over: it is not
     * the slice this scheduler uses, and it did not move if the tick rate
     * did. Real-time code sizes its work units from this value. */
    return (u64)SCHED_RR_TICKS * (1000000000ULL / SCHED_TICK_HZ);
}

u64 sched_proc_cpu_mask(process_t *p)
{
    if (!p) return 0;

    u64 mask = 0;
    irqflags_t irqf = spinlock_lock_irqsave(&g_sched_lock);
    for (thread_t *t = p->threads; t; t = t->proc_next) {
        if (t->state == THREAD_ZOMBIE) continue;
        if (t->cpu_id < 64) mask |= (1ULL << t->cpu_id);
        u32 q = __atomic_load_n(&t->rq_cpu, __ATOMIC_RELAXED);
        if (q < 64) mask |= (1ULL << q);
    }
    spinlock_unlock_irqrestore(&g_sched_lock, irqf);
    return mask;
}

u32 sched_nr_running(u32 cpu)
{
    if (cpu >= SMP_MAX_CPUS) return (u32)-1;
    return __atomic_load_n(&g_cpu_rq[cpu].nr_running, __ATOMIC_RELAXED);
}

u32 sched_cpu_of(const thread_t *t)
{
    return t ? t->cpu_id : (u32)-1;
}

process_t *proc_create(const char *name, phys_addr_t pml4_phys)
{
    process_t *proc = (process_t *)kzalloc(sizeof(process_t));
    if (!proc) return NULL;

    proc->pml4_phys = pml4_phys ? pml4_phys : vmm_kernel_space();
    proc->vma_lock = (spinlock_t)SPINLOCK_INIT;
    proc->fd_lock = (spinlock_t)SPINLOCK_INIT;

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
    proc->affinity_mask = (u64)-1; /* All CPUs allowed by default */

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

/* Bridge for tlb_shootdown_space() (arch/x86_64/mm/tlb.c): it needs to know
 * which CPUs might hold a stale translation for @space, and that record
 * (process_t::pcid_primed, kept current by every vmm_switch_proc() call —
 * see its doc comment in vmm.h) lives here, at the scheduler layer, not in
 * arch/x86_64/mm. Declared `extern` at its one call site rather than in a
 * shared header, matching how this codebase already crosses a couple of
 * other module boundaries (e.g. vfs_sync_all()'s `extern void ext2_sync()`).
 *
 * Deliberately only ever answers for the CURRENT cpu's own running process:
 * that covers the overwhelming majority of real calls (a process modifying
 * its own address space) with no locking at all — proc is pinned by the fact
 * that it is what is executing right now, so there's nothing to race with a
 * concurrent exit()/free(). Any other case (ptrace poking a different
 * process, tearing one down, a freshly cloned child no one has scheduled
 * yet) would need to search g_process_list under g_sched_lock, which the
 * caller may already hold indirectly through a lock-ordering path this
 * function has no way to know about — so instead of risking that, those
 * cases just get told "assume every CPU", which is always correct, just not
 * narrowed. */
u64 sched_tlb_current_space_mask(phys_addr_t space)
{
    cpu_info_t *cpu = smp_get_cpu();
    thread_t   *t   = cpu ? cpu->current_thread : NULL;
    process_t  *proc = t ? t->proc : NULL;

    if (!proc || proc->pml4_phys != space) return ~0ULL;

    u64 mask = __atomic_load_n(&proc->pcid_primed, __ATOMIC_RELAXED);
    return mask ? mask : ~0ULL;
}

/* Companion bridge to the above: the PCID tag that @space's translations are
 * cached under, so tlb_shootdown_user() can invalidate that one context on the
 * other cores instead of every translation they hold.
 *
 * Restricted exactly as sched_tlb_current_space_mask() is, and for exactly the
 * same reason: it answers only for the address space this CPU is running,
 * which needs no locking because that process is what is executing right now.
 * Anything else gets 0, which is the kernel's own tag and which the caller
 * reads as "unknown — flush everything". Narrowing is an optimisation; 0 is
 * always a correct answer. */
u16 sched_tlb_space_pcid(phys_addr_t space)
{
    cpu_info_t *cpu = smp_get_cpu();
    thread_t   *t   = cpu ? cpu->current_thread : NULL;
    process_t  *proc = t ? t->proc : NULL;

    if (!proc || proc->pml4_phys != space) return 0;
    return proc->pcid;
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
                /* The reparented zombie is now the subreaper's to collect, so
                 * wake it the same way a normal child exit does. Calling
                 * enqueue_ready() directly here (what this used to do) skipped
                 * notify_waiter_locked()'s state dispatch, so a waiter caught
                 * mid-context-switch in THREAD_BLOCKED_PENDING got linked into
                 * a run queue while another CPU was still switching away from
                 * it — and a waiter already zombified got resurrected. */
                if ((*pproc)->is_zombie && reaper)
                    notify_waiter_locked(reaper, SIGCHLD);
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

    /* Drop this PID's /proc directory from the dentry cache. procfs only
     * creates one for a live PID, but a cached name is never revalidated,
     * so without this /proc/<pid> outlives the process as a directory full
     * of empty files — which reads, to anything asking whether the process
     * is still there, as "yes". */
    extern void procfs_pid_exited(u32 pid);
    procfs_pid_exited(proc->pid);

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

    /* An AIO context created with io_setup(2) and never io_destroy(2)'d
     * holds a completion ring that nothing else will ever free. */
    extern void aio_process_exit(process_t *proc);
    aio_process_exit(proc);

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
    if (proc->io_bitmap) {
        kfree(proc->io_bitmap);
        proc->io_bitmap = NULL;
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

/*
 * Hot cache of stacks kept *fully mapped* between a thread's death and the
 * next thread's birth.
 *
 * Recycling only the VA slot still made every thread exit call
 * vmm_unmap_range(), and that ends in tlb_shootdown_space(): a broadcast IPI
 * to every other core followed by a spin until each one acknowledges. A
 * process winding down N threads paid N of those round-trips back to back,
 * serialised — it is the single most expensive thing on the thread-exit path,
 * and it is what produced the "[TLB] shootdown to CPUn stuck … resending IPI"
 * bursts during the threaded tests.
 *
 * Leaving the mapping in place removes the need for it entirely. The shootdown
 * exists because re-pointing a live kernel VA at a *different* physical frame
 * leaves other cores holding a stale global translation; a cached slot keeps
 * the exact same VA→frame mapping it always had, so there is nothing stale to
 * evict and nothing to broadcast. Allocation off this cache touches neither
 * the PMM nor the page tables — it only re-zeroes the pages, which is what the
 * map path did anyway and is far cheaper than one IPI round-trip.
 *
 * Capped so the pinned memory stays bounded (32 * 16 KB = 512 KB); past that
 * kstack_free() falls back to the unmap-and-release path below.
 */
#define KSTACK_HOT_CACHE   32
static u64 g_kstack_hot[KSTACK_HOT_CACHE];
static u32 g_kstack_hot_count;

/* Returns the VA of the lowest mapped stack page (the guard page sits at
 * base - PAGE_SIZE), or 0 on out-of-memory. */
static u64 kstack_alloc(void)
{
    irqflags_t f = spinlock_lock_irqsave(&g_kstack_lock);

    /* Already-mapped stack from a thread that just exited: no PMM allocation,
     * no page-table edit, no TLB shootdown. */
    if (g_kstack_hot_count > 0) {
        u64 hot = g_kstack_hot[--g_kstack_hot_count];
        spinlock_unlock_irqrestore(&g_kstack_lock, f);
        hw_clear_pages((void *)hot, (size_t)KSTACK_PAGES);
        return hot;
    }

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
            /* Give the slot index back. Everything else this path allocated is
             * released above, but the slot was consumed before the first
             * pmm_alloc_page() and nothing else remembers it: dropping it here
             * retires one guarded slot permanently. Under memory pressure —
             * precisely when this path runs — thread creation fails repeatedly,
             * so the guarded area drains a slot per failure until it is empty
             * and every later stack silently falls through to the unguarded
             * fallback below. */
            irqflags_t rf = spinlock_lock_irqsave(&g_kstack_lock);
            if (g_kstack_free_count < KSTACK_FREE_CACHE)
                g_kstack_free_list[g_kstack_free_count++] = (u32)slot;
            spinlock_unlock_irqrestore(&g_kstack_lock, rf);
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
        /* Park it fully mapped for the next thread if there is room — see the
         * KSTACK_HOT_CACHE comment. This is the common case on any workload
         * that creates and destroys threads, and it costs nothing at all. */
        irqflags_t hf = spinlock_lock_irqsave(&g_kstack_lock);
        if (g_kstack_hot_count < KSTACK_HOT_CACHE) {
            g_kstack_hot[g_kstack_hot_count++] = stack_base;
            spinlock_unlock_irqrestore(&g_kstack_lock, hf);
            return;
        }
        spinlock_unlock_irqrestore(&g_kstack_lock, hf);

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
 * Map a process's nice value onto the CFS load weight this scheduler uses.
 *
 * thread->priority holds the weight: vruntime advances by
 * NICE_0_WEIGHT * SCHED_VTIME_UNIT / weight per tick, so a *larger* weight
 * makes the virtual clock run slower and the leftmost-first tree keeps
 * choosing that thread. The values are Linux's, so a given nice level buys
 * the same share here as it does there.
 *
 * SCHED_FIFO and SCHED_RR do not go through this at all — they are a separate
 * run queue that outranks the tree entirely (see sched_rt_prio_for()).
 */
u32 sched_weight_for(const process_t *proc)
{
    if (!proc) return NICE_0_WEIGHT;

    if (proc->sched_policy == SCHED_IDLE) return WEIGHT_IDLEPRIO;

    s32 nice = proc->prio_nice;
    if (nice < -20) nice = -20;
    if (nice >  19) nice =  19;

    u32 w = g_nice_to_weight[nice + 20];

    /* SCHED_BATCH is CFS with the same weight; what distinguishes it in Linux
     * is that it is never treated as interactive on wakeup. This scheduler has
     * no wakeup bonus to withhold, so the two coincide — recorded faithfully,
     * acted on identically. */
    return w ? w : 1;
}

/*
 * Real-time priority implied by a process's policy.
 *
 * SCHED_FIFO and SCHED_RR run on a separate per-CPU queue that is drained
 * before the CFS tree is even looked at, and a real-time thread only ever
 * yields to a *higher* real-time priority. The difference between the two
 * policies is what happens when the slice runs out: SCHED_RR gives up the CPU
 * to an equal-priority peer (sched_tick() counts the slice down), SCHED_FIFO
 * runs until it blocks or is preempted by something higher.
 *
 * This used to collapse to "CFS weight 1", which was not real-time in any
 * sense that matters: a FIFO thread still lost the CPU to a nice-19 thread as
 * soon as its vruntime drifted ahead, and priority levels 1..99 were all the
 * same. Audio, input handling and anything else that asks for SCHED_FIFO asks
 * because it needs the latency bound, and an approximation of it is worse
 * than useless — it reports success and then misses the deadline.
 */
u32 sched_rt_prio_for(const process_t *proc)
{
    if (!proc) return 0;
    if (proc->sched_policy != SCHED_FIFO && proc->sched_policy != SCHED_RR) return 0;

    s32 p = proc->sched_rt_prio;
    if (p < 1)  p = 1;
    if (p > 99) p = 99;
    return (u32)p;
}

/* Re-apply the process's weight and class to every thread it currently has.
 * Called after setpriority()/sched_setscheduler() change the policy or nice
 * value.
 *
 * A thread that is queued has to be moved: the RT queue and the CFS tree are
 * different data structures, and rq_try_remove_on() decides which one a
 * thread is on by looking at rt_priority. Changing that field under a queued
 * thread would make it unremovable from the queue it is actually on — so the
 * thread is dequeued under its old class and re-enqueued under its new one.
 */
void sched_apply_weight(process_t *proc)
{
    if (!proc) return;

    u32 w  = sched_weight_for(proc);
    u32 rt = sched_rt_prio_for(proc);

    irqflags_t irqf = spinlock_lock_irqsave(&g_sched_lock);
    for (thread_t *t = proc->threads; t; t = t->proc_next) {
        bool requeue = false;

        if (t->rt_priority != rt && __atomic_load_n(&t->rq_cpu, __ATOMIC_RELAXED) != (u32)-1) {
            rq_remove_thread_locked(t);
            requeue = true;
        }

        t->priority    = w;
        t->rt_priority = rt;
        t->rr_ticks_left = rt ? SCHED_RR_TICKS : 0;

        if (requeue) enqueue_ready(t);
    }
    spinlock_unlock_irqrestore(&g_sched_lock, irqf);
}

thread_t *thread_create_ex(process_t *proc, uintptr_t entry, uintptr_t arg, bool is_kernel, bool enqueue)
{
    thread_t *t = thread_alloc();
    if (!t) return NULL;

    irqflags_t irqf = spinlock_lock_irqsave(&g_sched_lock);
    t->tid = (proc && proc->threads == NULL) ? proc->pid : g_next_tid++;
    spinlock_unlock_irqrestore(&g_sched_lock, irqf);

    t->proc = proc ? proc : g_kernel_proc;
    t->state = THREAD_READY;
    t->vruntime = 0;
    t->affinity_mask = proc ? proc->affinity_mask : (u64)-1;
    t->cpu_id = (u32)-1;
    t->rq_cpu = (u32)-1;   /* not on any run queue yet (0 would name CPU 0) */
    /* Base CFS load weight is NICE_0_WEIGHT (1024). A process that has
     * changed its nice value, or picked a real-time policy, carries that onto
     * every thread it spawns — which is what POSIX requires of both
     * pthread_create() and fork(). */
    t->priority      = sched_weight_for(t->proc);
    t->rt_priority   = sched_rt_prio_for(t->proc);
    t->rr_ticks_left = t->rt_priority ? SCHED_RR_TICKS : 0;
    t->rt_next       = NULL;
    
    /* Allocate a guarded 16 KB kernel stack for this thread */
    u64 kstack_base = kstack_alloc();
    if (!kstack_base) {
        thread_free(t);
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
    u32 my_cpu = (cpu && cpu->cpu_id < SMP_MAX_CPUS) ? cpu->cpu_id : 0;
    runqueue_t *my_rq = &g_cpu_rq[my_cpu];

    for (;;) {
        if (__atomic_load_n(&rq_head(my_rq), __ATOMIC_ACQUIRE)) {
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
        /* Arm the wake-up watch on the local run queue head *before* the final test,
         * so a thread enqueued in the gap still breaks us out of the wait. On a
         * CPU without MONITOR/MWAIT this is a no-op and the STI;HLT below
         * provides the same guarantee via the wake-up IPI. */
        cpu_idle_arm(&rq_head(my_rq));
        if (__atomic_load_n(&rq_head(my_rq), __ATOMIC_ACQUIRE)) {
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
                        /* Same as in proc_destroy(): go through the state
                         * dispatch rather than enqueueing the waiter blind. */
                        if (child->is_zombie && reaper)
                            notify_waiter_locked(reaper, SIGCHLD);
                    }
                    child = child->next;
                }
                
                /* Take ownership of the whole thread list before the lock
                 * drops. sched_waitpid() can be reaping this same process on
                 * another CPU (a tracer collects a tracee it does not parent)
                 * and frees from proc->threads too; whoever detaches the list
                 * first owns every struct on it, so the other side finds an
                 * empty list instead of freeing the same thread twice. */
                thread_t *tlist = proc->threads;
                proc->threads = NULL;

                /* Unlock safely while freeing detached process resources */
                spinlock_unlock_irqrestore(&g_sched_lock, irqf);

                while (tlist) {
                    thread_t *next_t = tlist->proc_next;
                    /* Free guarded kernel stack + thread struct */
                    kstack_free(tlist->kernel_stack_base);
                    thread_free(tlist);
                    tlist = next_t;
                }

                /* Free VMM page tables, open handles, IPC channels, and
                 * process struct. Order as in sched_waitpid() — see the note
                 * there on why it cannot simply be swapped. */
                proc_destroy(proc);
                
                irqf = spinlock_lock_irqsave(&g_sched_lock);
                pproc = &g_process_list;
            } else {
                /* Process still alive — clean up any individual zombie threads.
                 * Every one of them is unlinked inside this single lock hold
                 * and freed afterwards from a private list. Dropping the lock
                 * once per thread and re-reading proc->threads (what this used
                 * to do) is not safe: kstack_free() blocks on a cross-CPU TLB
                 * shootdown, and in that window the process's last live thread
                 * can zombify, at which point sched_waitpid() reaps the process
                 * — freeing the rest of its threads and @proc itself. This loop
                 * would then walk freed memory and race waitpid to free the
                 * same thread_t, which is exactly the "kfree called on
                 * corrupted or non-kmalloc pointer (magic=0)" double free. */
                thread_t *dead = NULL;
                thread_t **pt = &proc->threads;
                while (*pt) {
                    thread_t *curr_t = *pt;
                    if (curr_t->state == THREAD_ZOMBIE) {
                        *pt = curr_t->proc_next;
                        /* proc_next doubles as the private free-list link */
                        curr_t->proc_next = dead;
                        dead = curr_t;
                    } else {
                        pt = &curr_t->proc_next;
                    }
                }

                if (!dead) {
                    pproc = &proc->next;
                    continue;
                }

                spinlock_unlock_irqrestore(&g_sched_lock, irqf);
                while (dead) {
                    thread_t *next_t = dead->proc_next;
                    kstack_free(dead->kernel_stack_base);
                    thread_free(dead);
                    dead = next_t;
                }
                irqf = spinlock_lock_irqsave(&g_sched_lock);
                /* @proc may have been reaped while the lock was down, so
                 * proc->next is no longer safe to follow — restart the sweep
                 * from the head. It terminates: the next pass over this
                 * process finds no zombie threads left (or does not find the
                 * process at all) and advances. */
                pproc = &g_process_list;
            }
        }
        spinlock_unlock_irqrestore(&g_sched_lock, irqf);
    }
}

void sched_init(void)
{
    for (u32 i = 0; i < SMP_MAX_CPUS; i++) {
        spinlock_init(&g_cpu_rq[i].lock);
        rq_tree_init(&g_cpu_rq[i].tree);
        g_cpu_rq[i].min_vruntime = 0;
        g_cpu_rq[i].nr_running = 0;
        g_cpu_rq[i].rt_head = NULL;
        g_cpu_rq[i].rt_nr_running = 0;
        /* Stagger the first balance so neighbouring CPUs do not all reach for
         * each other's run-queue locks on the same tick. */
        g_cpu_rq[i].next_balance_tick = i % SCHED_BALANCE_INTERVAL_TICKS;
    }

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
    gdt_set_iopb(cpu->cpu_id, next->proc ? next->proc->io_bitmap : NULL);

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

/*
 * Should @prev give the CPU to @next?
 *
 * For a CFS incumbent the answer is always yes: dequeue_ready() returned the
 * leftmost thread, which by definition has the smallest virtual runtime, and
 * this is also the path sched_yield(2) uses, where giving way is the point.
 *
 * For a real-time incumbent it is not. dequeue_ready() pops the head of the
 * real-time queue without knowing what is currently running, so a SCHED_FIFO
 * thread at priority 50 could be switched out for one at priority 10 — the
 * exact inversion the real-time classes exist to prevent. The incumbent keeps
 * the CPU unless @next genuinely outranks it, or unless the two are equal and
 * the incumbent has used up its round-robin slice (rr_ticks_left == 0, set by
 * sched_tick() and by an explicit sched_yield(2)), which is what lets peers at
 * the same priority take turns.
 */
static bool sched_should_switch(const thread_t *prev, const thread_t *next)
{
    if (!prev->rt_priority) return true;
    if (!next->rt_priority) return false;
    if (next->rt_priority != prev->rt_priority)
        return next->rt_priority > prev->rt_priority;
    return prev->rr_ticks_left == 0;
}

/* Put a thread this CPU just dequeued back where it came from, without going
 * through enqueue_ready()'s placement logic — it was already placed here, and
 * re-running the search could ship it to another CPU for no reason. */
static void rq_push_back_local(u32 cpu_id, thread_t *t)
{
    runqueue_t *rq = &g_cpu_rq[cpu_id];
    spinlock_lock(&rq->lock);
    if (t->rt_priority) {
        rt_enqueue_locked(rq, t, true);
    } else {
        if (t->vruntime < rq->min_vruntime) t->vruntime = rq->min_vruntime;
        rq_tree_insert(&rq->tree, t);
        rq->nr_running++;
    }
    t->rq_cpu = cpu_id;
    t->state = THREAD_READY;
    spinlock_unlock(&rq->lock);
}

void sched_yield(void)
{
    cpu_info_t *cpu = smp_get_cpu();
    if (!cpu || !cpu->current_thread) return;

    irqflags_t irqf = spinlock_lock_irqsave(&g_sched_lock);

    thread_t *prev = cpu->current_thread;
    thread_t *next = dequeue_ready();

    /* Real-time priority inversion guard — see sched_should_switch(). A
     * thread on its way out (DYING) has to leave regardless, and the idle
     * thread yields to anything. */
    if (next && prev->state != THREAD_DYING &&
        prev != g_idle_threads[cpu->cpu_id] &&
        !sched_should_switch(prev, next)) {
        rq_push_back_local(cpu->cpu_id, next);
        prev->state = THREAD_RUNNING;
        spinlock_unlock_irqrestore(&g_sched_lock, irqf);
        return;
    }

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
    gdt_set_iopb(cpu->cpu_id, next->proc ? next->proc->io_bitmap : NULL);

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

/* ── Load average ──────────────────────────────────────────────────────────
 * fs/procfs.c's /proc/loadavg used to hand back a fixed "0.12 0.08 0.03" —
 * numbers formatted to look like real telemetry that never actually moved no
 * matter how loaded the system got. Real load average is a genuinely
 * time-decayed history, not something computable at the moment /proc/loadavg
 * happens to be read, so it has to be sampled periodically regardless of
 * when (or whether) anything ever reads it. This does the same thing Linux
 * does: sample the number of runnable threads every 5 seconds and
 * exponentially decay three running averages with 1/5/15-minute time
 * constants, fixed-point with 11 fractional bits (Linux's FSHIFT/FIXED_1).
 * Sampling piggybacks on sched_tick()'s existing 100 Hz BSP timer instead of
 * a separate kernel thread, the same way the ARP/TCP/IPv4 "once a second"
 * timers just below it already do. */
#define LOAD_FSHIFT   11
#define LOAD_FIXED_1  (1U << LOAD_FSHIFT)
#define LOAD_EXP_1    1884U   /* 1/exp(5s/60s), Q11 fixed-point */
#define LOAD_EXP_5    2014U   /* 1/exp(5s/300s) */
#define LOAD_EXP_15   2037U   /* 1/exp(5s/900s) */
#define LOAD_SAMPLE_TICKS 500 /* 5s at the real, HPET-calibrated 100Hz tick */

static u64 g_load_avg[3] = {0, 0, 0}; /* Q11 fixed-point, like Linux's avenrun[] */

static u32 sched_count_runnable_locked(void)
{
    u32 count = 0;
    for (process_t *p = g_process_list; p; p = p->next) {
        for (thread_t *t = p->threads; t; t = t->proc_next) {
            if (t->state == THREAD_READY || t->state == THREAD_RUNNING) count++;
        }
    }
    return count;
}

/* Called once every LOAD_SAMPLE_TICKS from sched_tick() on the BSP. */
static void sched_sample_load(void)
{
    sched_lock();
    u32 active = sched_count_runnable_locked();
    sched_unlock();

    u64 active_fixed = (u64)active * LOAD_FIXED_1;
    g_load_avg[0] = (g_load_avg[0] * LOAD_EXP_1  + active_fixed * (LOAD_FIXED_1 - LOAD_EXP_1))  >> LOAD_FSHIFT;
    g_load_avg[1] = (g_load_avg[1] * LOAD_EXP_5  + active_fixed * (LOAD_FIXED_1 - LOAD_EXP_5))  >> LOAD_FSHIFT;
    g_load_avg[2] = (g_load_avg[2] * LOAD_EXP_15 + active_fixed * (LOAD_FIXED_1 - LOAD_EXP_15)) >> LOAD_FSHIFT;
}

void sched_tick(pt_regs_t *regs)
{
    
    /* Acknowledge the timer interrupt immediately so the LAPIC can send more
     * even if we context switch away from this thread. */
    extern void lapic_eoi(void);
    lapic_eoi();

    /* In TSC-deadline mode the LAPIC timer is one-shot by construction, so
     * the next tick has to be armed from here or this CPU never gets another
     * one. A no-op when the timer is running in classic periodic mode. */
    lapic_timer_rearm();

    cpu_info_t *cpu = smp_get_cpu();
    if (!cpu || !cpu->current_thread) return;

    u64 current_ticks;
    if (cpu->is_bsp) {
        current_ticks = __atomic_add_fetch(&g_system_ticks, 1, __ATOMIC_RELAXED);
        /* Accumulate the clocksource and republish the vvar page. First,
         * so everything below this tick sees the updated clocks. */
        timekeeping_tick();
        extern void net_poll(void);
        net_poll();

        /* ARP cache expiry/retry, TCP handshake retransmission, and IPv4
         * fragment reassembly expiry all work in whole seconds (see
         * ARP_RETRY_TIMEOUT, TCP_RTX_BASE_TIMEOUT, IP_REASSEMBLY_TIMEOUT),
         * not in LAPIC ticks — calling them on every tick would fire them
         * ~100x too fast at the 100 Hz lapic_timer_start(100) in
         * kernel/main.c. Gating on that same 100 recovers "once a second"
         * without any of them needing to know the tick rate itself. */
        if (current_ticks % 100 == 0) {
            extern void arp_timer_tick(void);
            arp_timer_tick();
            extern void tcp_timer_tick(void);
            tcp_timer_tick();
            extern void ipv4_timer_tick(void);
            ipv4_timer_tick();
        }

        /* Load average sample, every 5s (LOAD_SAMPLE_TICKS) -- see the
         * sched_sample_load() comment above for why this can't just be
         * computed on demand when /proc/loadavg is read. */
        if (current_ticks % LOAD_SAMPLE_TICKS == 0) {
            sched_sample_load();
        }
    } else {
        current_ticks = __atomic_load_n(&g_system_ticks, __ATOMIC_RELAXED);
    }

    KTRACE_CALL("sched_tick", current_ticks);

    cpu->ticks++;
    thread_t *curr = cpu->current_thread;

    /* Charge this tick against the thread's virtual clock, scaled by its load
     * weight: a thread with twice the weight accrues vruntime half as fast,
     * so the leftmost-first tree hands it twice as much real time. Real-time
     * threads have no virtual clock — their ordering is their priority — so
     * they are excluded and keep a vruntime of whatever they last had as a
     * CFS thread, which is what they get back if their policy changes. */
    if (!curr->rt_priority) {
        u32 w = curr->priority ? curr->priority : NICE_0_WEIGHT;
        curr->vruntime += ((u64)NICE_0_WEIGHT * SCHED_VTIME_UNIT) / w;
    }

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
    u32 my_cpu = (cpu->cpu_id < SMP_MAX_CPUS) ? cpu->cpu_id : 0;
    runqueue_t *my_rq = &g_cpu_rq[my_cpu];
    bool rq_nonempty = __atomic_load_n(&rq_head(my_rq), __ATOMIC_RELAXED) != NULL ||
                       __atomic_load_n(&my_rq->rt_nr_running, __ATOMIC_RELAXED) != 0;

    bool should_preempt = false;

    /*
     * SCHED_RR slice accounting. A round-robin thread that has used its slice
     * gives way to an equal-priority peer; with no peer waiting it simply
     * gets a fresh slice and carries on, which is what distinguishes RR from
     * FIFO without turning it into a context switch every 100 ms for no
     * reason. SCHED_FIFO threads (rr_ticks_left == 0 by construction, since
     * dequeue only arms the slice for threads that have one) are never
     * charged here: they run until they block or something higher arrives.
     */
    if (curr->rt_priority && curr->proc && curr->proc->sched_policy == SCHED_RR) {
        if (curr->rr_ticks_left > 0) curr->rr_ticks_left--;
        if (curr->rr_ticks_left == 0) {
            spinlock_lock(&my_rq->lock);
            /* Only yield if a peer at the same priority is actually waiting.
             * Leaving rr_ticks_left at 0 is what tells enqueue_ready() to put
             * this thread at the *back* of its priority rather than the
             * front — the difference between round-robin and FIFO — and
             * dequeue_ready() re-arms the slice when it next runs. With no
             * peer waiting, the slice is simply renewed in place. */
            if (my_rq->rt_head && my_rq->rt_head->rt_priority >= curr->rt_priority)
                should_preempt = true;
            else
                curr->rr_ticks_left = SCHED_RR_TICKS;
            spinlock_unlock(&my_rq->lock);
        }
    }

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

        spinlock_lock(&my_rq->lock);
        if (my_rq->rt_head) {
            /* Anything real-time and runnable outranks a CFS thread outright,
             * and outranks a real-time thread of strictly lower priority. */
            should_preempt = should_preempt ||
                             !curr->rt_priority ||
                             my_rq->rt_head->rt_priority > curr->rt_priority;
        } else if (!curr->rt_priority) {
            should_preempt = should_preempt ||
                             (rq_head(my_rq) &&
                              (curr == g_idle_threads[cpu->cpu_id] ||
                               rq_head(my_rq)->vruntime < curr->vruntime));
        }
        spinlock_unlock(&my_rq->lock);
        spinlock_unlock(&g_sched_lock);
    }

    /* Pull work from a lopsided neighbour. Deliberately after the preemption
     * decision above: a thread pulled in now is picked up by the reschedule
     * this tick is about to request, or by the next one. */
    sched_balance_tick(my_cpu, current_ticks);

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
    gdt_set_iopb(cpu->cpu_id, next->proc ? next->proc->io_bitmap : NULL);

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
    gdt_set_iopb(cpu->cpu_id, next->proc ? next->proc->io_bitmap : NULL);
    
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
    /* The callers hold bare thread_t* wait slots that a thread killed while
     * blocked never got to clear, so this is reached with a retired struct.
     * thread_alloc() keeps that memory typed, which makes the stamp readable
     * and this check sufficient. */
    if (t->magic != THREAD_MAGIC) return;
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

/*
 * Wake every thread of @p that is waiting, in one pass under the scheduler
 * lock.
 *
 * Callers outside this file cannot do this themselves: walking p->threads
 * needs g_sched_lock (the reaper and the exit paths relink that list from
 * other CPUs), but sched_unblock() takes g_sched_lock itself and sched_lock()
 * is deliberately non-reentrant, so a locked walk calling it per thread would
 * deadlock and an unlocked one races the list. fs/userfaultfd.c was doing the
 * unlocked version.
 *
 * It also catches THREAD_BLOCKED_PENDING, which a hand-rolled
 * `if (state == THREAD_BLOCKED) sched_unblock(t)` filter silently drops: a
 * thread caught between sched_block() and sched_post_switch() is not yet
 * THREAD_BLOCKED, so it never sees the wake-up and parks forever.
 */
void sched_wake_proc_waiters(process_t *p)
{
    if (!p) return;
    irqflags_t irqf = spinlock_lock_irqsave(&g_sched_lock);
    for (thread_t *t = p->threads; t; t = t->proc_next) {
        if (t->state == THREAD_BLOCKED) {
            enqueue_ready(t);
        } else if (t->state == THREAD_SLEEPING) {
            sleep_queue_remove_locked(t);
            enqueue_ready(t);
        } else if (t->state == THREAD_BLOCKED_PENDING ||
                   t->state == THREAD_SLEEPING_PENDING) {
            t->unblock_pending = true;
        }
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
    if (w->magic != THREAD_MAGIC) return;
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
            rq_remove_thread_locked(t);
        } else if (t->state == THREAD_SLEEPING) {
            for (thread_t **pp = &g_sleep_queue; *pp; pp = &(*pp)->next)
                if (*pp == t) { *pp = t->next; break; }
        }
        thread_zombify_locked(t);
    }
    spinlock_unlock_irqrestore(&g_sched_lock, irqf);
}

void sched_dethread_wait(void)
{
    thread_t *self = sched_current_thread();
    if (!self || !self->proc) return;

    /* Bounded by real elapsed time (system ticks), not a raw PAUSE-loop
     * iteration count: PAUSE latency varies by roughly an order of
     * magnitude across x86-64 microarchitectures, so a fixed spin count is
     * not a fixed duration — it under- or over-waits depending on the CPU
     * this happens to run on. 500 ticks is 5s at the 100 Hz rate main.c
     * programs the LAPIC timer for (lapic_timer_start(100)), matching how
     * ipc_wait_tick()/mq_deadline_ticks() already bound their waits. */
    u64 deadline = sched_get_ticks() + 500;
    while (sched_get_ticks() < deadline) {
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
        /* Only genuinely dead siblings. sched_dethread_wait() gives up after
         * 5 s, and taking every non-self thread unconditionally (what this used
         * to do) meant that on a timeout we freed the kernel stack of a thread
         * still executing on another core. Leaving a straggler linked costs
         * nothing — the reaper sweep collects it once it does zombify. */
        if (t != self && t->state == THREAD_ZOMBIE) {
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
        thread_free(to_free);
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
    gdt_set_iopb(cpu->cpu_id, next->proc ? next->proc->io_bitmap : NULL);

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

            /* BUG-Q fix: verify every thread is genuinely ZOMBIE before freeing.
             * proc->is_zombie is set by sched_post_switch only after all threads
             * have transitioned to THREAD_ZOMBIE and no CPU holds prev_thread on
             * any of them, so this check should always pass — but be defensive
             * against future code changes that relax that invariant.
             *
             * Checked *before* the unlink below: the earlier order took the
             * child off g_process_list and only then bailed out to retry, so
             * the retry could no longer find it — the caller got -ECHILD and
             * the process and all its threads leaked for good. */
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

            /* Remove zombie child from process list */
            process_t **pp = &g_process_list;
            while (*pp) {
                if (*pp == zombie_child) {
                    *pp = zombie_child->next;
                    break;
                }
                pp = &(*pp)->next;
            }

            /* Detach the thread list under the lock and free from the private
             * copy below. Taking it off g_process_list is not enough on its
             * own: the reaper sweep may already be part-way through this
             * process's threads, and it drops g_sched_lock while it frees
             * (kstack_free() waits on a cross-CPU TLB shootdown). Reading
             * zombie_child->threads after the unlock therefore raced the
             * reaper for the same thread_t and freed it twice — the kfree()
             * "magic=0 (already freed)" panic. */
            thread_t *tlist = zombie_child->threads;
            zombie_child->threads = NULL;

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

            /* Free child threads, then the process itself.
             *
             * NOTE: this order is load-bearing. proc_destroy() closes the
             * process's sockets, pipes and IPC channels, and each of those
             * wakes whatever thread was parked on it through a bare thread_t*
             * the object kept (sock->rx_wait_thread, pipe readers, ipc
             * senders). A thread killed while blocked there is zombified in
             * place and never clears that back-pointer, so those wake-ups do
             * still name a thread_t retired just above. That is survivable
             * only because thread_free() keeps the struct typed and stamped —
             * see thread_alloc(). Running proc_destroy() *first* to close the
             * window outright was tried and deadlocks the reap path, so do not
             * "fix" this by swapping the two. */
            while (tlist) {
                thread_t *next_t = tlist->proc_next;
                kstack_free(tlist->kernel_stack_base);
                thread_free(tlist);
                tlist = next_t;
            }
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
            thread_zombify_locked(t);
        } else if (t->state == THREAD_READY) {
            /* Remove from per-CPU ready queue */
            rq_remove_thread_locked(t);
            thread_zombify_locked(t);
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

/* sched_get_loadavg() — fills out[0..2] with the 1/5/15-minute load average
 * in Q11 fixed-point (out[i] / 2048.0 gives the real value), for
 * fs/procfs.c's /proc/loadavg to format. See the sched_sample_load() comment
 * above sched_tick() for how these are actually computed. */
void sched_get_loadavg(u32 out[3])
{
    out[0] = (u32)g_load_avg[0];
    out[1] = (u32)g_load_avg[1];
    out[2] = (u32)g_load_avg[2];
}

/* sched_get_last_pid() — the most recently allocated PID, for the last field
 * of /proc/loadavg (real Linux reports the PID of the most recently created
 * process there; this was hardcoded to a fixed "12" before). */
u32 sched_get_last_pid(void)
{
    return g_next_pid > 1 ? g_next_pid - 1 : 1;
}

/* sched_get_context_switches() — total switch_to_asm() invocations since
 * boot, for /proc/stat's "ctxt" line. */
u64 sched_get_context_switches(void)
{
    return __atomic_load_n(&g_context_switches, __ATOMIC_RELAXED);
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

/*
 * Make @t obey a freshly assigned affinity @mask. Caller holds g_sched_lock
 * and has already stored the mask on the thread.
 *
 * Both sched_set_thread_affinity() and sched_set_proc_affinity() carried their
 * own copy of this, each with an open-coded run-queue unlink that knew nothing
 * about the queue's own bookkeeping or thread_t::rq_cpu, and could leave it
 * inconsistent. Going through rq_try_remove_on() keeps that in one place, and
 * reading the queue out of rq_cpu goes straight to the one run queue the
 * thread is on instead of scanning all 64.
 */
static void thread_apply_affinity_locked(thread_t *t, u64 mask, u32 ncpus)
{
    if (t->state == THREAD_RUNNING && t->cpu_id < SMP_MAX_CPUS) {
        /* On a CPU the mask no longer allows: bounce it off that core. */
        if (!(mask & (1ULL << t->cpu_id))) {
            if (t->cpu_id == smp_current_cpu_id()) {
                cpu_info_t *cpu = smp_get_cpu();
                if (cpu) cpu->needs_reschedule = true;
            } else {
                smp_send_reschedule(t->cpu_id);
            }
        }
        return;
    }

    if (t->state != THREAD_READY) return;

    /* Queued on a run queue the mask excludes: pull it off and let
     * enqueue_ready() place it on one the mask does allow. */
    u32 q = t->rq_cpu;
    bool pulled = false;
    if (q < ncpus) {
        if (mask & (1ULL << q)) return;     /* already on an allowed queue */
        pulled = rq_try_remove_on(q, t);
    } else {
        /* Hint unusable — only the excluded queues can need draining. */
        for (u32 i = 0; i < ncpus && !pulled; i++)
            if (!(mask & (1ULL << i))) pulled = rq_try_remove_on(i, t);
    }
    if (pulled) enqueue_ready(t);
}

int sched_set_thread_affinity(thread_t *t, u64 mask)
{
    if (!t) return -ESRCH;
    u32 ncpus = smp_cpu_count();
    if (ncpus == 0) ncpus = 1;
    if (ncpus > SMP_MAX_CPUS) ncpus = SMP_MAX_CPUS;
    u64 online_mask = (ncpus >= 64) ? ~0ULL : ((1ULL << ncpus) - 1);
    if ((mask & online_mask) == 0) return -EINVAL;

    irqflags_t irqf = spinlock_lock_irqsave(&g_sched_lock);
    t->affinity_mask = mask;
    thread_apply_affinity_locked(t, mask, ncpus);
    spinlock_unlock_irqrestore(&g_sched_lock, irqf);
    return 0;
}

int sched_set_proc_affinity(process_t *proc, u64 mask)
{
    if (!proc) return -ESRCH;
    u32 ncpus = smp_cpu_count();
    if (ncpus == 0) ncpus = 1;
    if (ncpus > SMP_MAX_CPUS) ncpus = SMP_MAX_CPUS;
    u64 online_mask = (ncpus >= 64) ? ~0ULL : ((1ULL << ncpus) - 1);
    if ((mask & online_mask) == 0) return -EINVAL;

    irqflags_t irqf = spinlock_lock_irqsave(&g_sched_lock);
    proc->affinity_mask = mask;
    for (thread_t *t = proc->threads; t; t = t->proc_next) {
        t->affinity_mask = mask;
        thread_apply_affinity_locked(t, mask, ncpus);
    }
    spinlock_unlock_irqrestore(&g_sched_lock, irqf);
    return 0;
}


