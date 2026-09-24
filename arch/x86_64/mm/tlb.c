/* ============================================================================
 * AzamiOS — Cross-CPU TLB Shootdown (x86_64)
 * File: arch/x86_64/mm/tlb.c
 * ============================================================================ */

#include "tlb.h"
#include "../cpu/msr.h"
#include "../cpu/spinlock.h"
#include "vmm.h"
#include "../cpu/cpu.h"
#include "../cpu/smp.h"
#include "../cpu/lapic.h"
#include "../../../include/azami/defs.h"
#include "../../../drivers/char/console.h"

#define TLB_SHOOTDOWN_VECTOR  251

/* TSC cycles to wait for a target's ack before concluding it is late enough
 * to be worth re-sending the IPI rather than just still in flight. This used
 * to be a fixed cpu_pause() iteration count, which is exactly the anti-pattern
 * sched_dethread_wait() already warns about elsewhere in this codebase: PAUSE
 * latency varies by roughly an order of magnitude across x86-64
 * microarchitectures, so a fixed spin count is not a fixed duration — on a
 * fast real CPU the same iteration count burns through in a fraction of the
 * wall-clock time it took under QEMU TCG (where this budget was tuned), so
 * the wait gave up and logged "stuck" over perfectly ordinary brief
 * contention (e.g. the target CPU running a short IRQs-off critical section)
 * far more eagerly than intended. TSC cycles are a hardware constant
 * regardless of PAUSE cost, so this bounds the wait in real time instead. 50M
 * cycles is tens of milliseconds on anything built this century — short
 * enough that a genuinely dropped IPI (see the wait loop's comment) still
 * gets retried well under a second, long enough that ordinary contention
 * resolves before the timeout ever fires. */
#define TLB_SHOOTDOWN_RESEND_CYCLES  50000000ull

/* Budget for the *first* resend only, roughly a millisecond on anything this
 * boots on.
 *
 * The long budget above is tuned for the case it describes: a target that is
 * merely slow to get to the handler, where resending early would be noise. It
 * is the wrong budget for the other case the wait loop below documents — an
 * IPI the target's LAPIC dropped outright because our vector was still in
 * service from a request moments earlier. That happens specifically when two
 * shootdowns land back to back, which is exactly what a caller making a
 * related series of page-table edits produces, and no amount of waiting fixes
 * it: the delivery is gone, and the only thing that can help is another one.
 * Spending a full 15 ms discovering that, sixteen times over, is where the
 * ~0.5 s "[TLB] shootdown to CPUn stuck" episodes during boot came from.
 *
 * Trying once quickly costs a single redundant IPI when the target was just
 * slow — the handler is idempotent, so a redundant delivery resolves to the
 * same "flush, publish req" it would have done anyway — and recovers a
 * genuinely dropped one in about a millisecond instead of fifteen. Subsequent
 * resends fall back to the long budget, so a target that is really wedged is
 * still not hammered. */
#define TLB_SHOOTDOWN_FIRST_RESEND_CYCLES  3000000ull

/* Request/completion counters, one pair per CPU.
 *
 * An initiator bumps g_tlb_req[target] and remembers the value it produced;
 * the target snapshots g_tlb_req[self] *before* flushing and stores that
 * snapshot into g_tlb_done[self] afterwards. So g_tlb_done[i] >= my_ticket
 * proves a flush that *started* after my page-table edit has finished — which
 * is the property the caller actually needs, and it holds even when several
 * CPUs shoot down at once or a stray IPI arrives. No lock is involved, so the
 * handler can never be blocked by whoever is waiting on it. */
typedef struct {
    volatile u64 req;
    volatile u64 done;

    /* What the pending request wants invalidated, written by the initiators and
     * consumed by this CPU's handler.
     *
     * `lock` guards these fields *and* the req bump, which is the whole trick:
     * a request is only counted in `req` after its scope has been merged in
     * here, so a handler that reads `req` and this descriptor under the same
     * lock is guaranteed that everything it is about to acknowledge is
     * something it is about to flush. Several initiators can pile onto one IPI
     * safely.
     *
     * `flush_all` means "discard everything", which is what this handler did
     * unconditionally before. `pcid == 0` with flush_all clear means nothing is
     * queued — what a duplicate or resent IPI sees — and is treated the same
     * way. 0 is never a real user PCID (it is the kernel's tag), so it doubles
     * as the "no precise scope" sentinel without needing a third field. */
    spinlock_t   lock;
    u16          pcid;
    bool         flush_all;

    u8           _pad[128 - 2 * sizeof(u64) - sizeof(spinlock_t)
                          - sizeof(u16) - sizeof(bool)];
} __attribute__((aligned(64))) tlb_cpu_state_t;

static tlb_cpu_state_t g_tlb_cpu[SMP_MAX_CPUS];

/* First address of the kernel half (PML4 entry 256 — the entries
 * vmm_clone_address_space() shares rather than copies). Everything below it is
 * a user mapping, and user mappings are the only ones a single-context
 * invalidation is guaranteed to reach. */
#define TLB_USER_VA_LIMIT   0x0000800000000000ULL

/*
 * Merge one request's scope into a target's pending descriptor. Caller holds
 * st->lock. Every case that cannot be answered precisely escalates to
 * flush_all rather than guessing.
 */
static void tlb_queue_pcid(tlb_cpu_state_t *st, u16 pcid)
{
    if (st->flush_all) return;

    if (pcid == 0) {                    /* caller could not narrow it */
        st->flush_all = true;
        st->pcid      = 0;
        return;
    }
    if (st->pcid == 0) {                /* nothing queued yet */
        st->pcid = pcid;
        return;
    }
    if (st->pcid != pcid) {             /* two address spaces in one request */
        st->flush_all = true;
        st->pcid      = 0;
    }
}

void tlb_flush_local_global(void)
{
    /* INVPCID "all contexts including globals" is one instruction and does not
     * disturb CR3 or CR4; the CR4.PGE toggle is the portable fallback. */
    if (g_invpcid_enabled) {
        invpcid(INVPCID_ALL_GLOBAL, 0, 0);
        return;
    }
    u64 cr4 = read_cr4();
    if (cr4 & CR4_PGE) {              /* PGE set: toggling it evicts globals */
        /* Through cpu_write_cr4() like every other CR4 write, so the pinned
         * protection bits survive the toggle. PGE itself is deliberately not
         * in the pin mask, which is what makes this legal at all. */
        cpu_write_cr4(cr4 & ~CR4_PGE);
        cpu_write_cr4(cr4);
    } else {
        write_cr3(read_cr3());
    }
}

static inline bool irqs_enabled(void)
{
    u64 flags;
    __asm__ volatile("pushfq; popq %0" : "=r"(flags));
    return (flags & 0x200ULL) != 0;
}

/* sched.c owns the per-process record of which cores have ever loaded a given
 * address space (process_t::pcid_primed, kept current by every vmm_switch_
 * proc() call — see its doc comment in vmm.h); this is the one-function
 * bridge tlb_shootdown_space() uses to read it without arch/x86_64/mm pulling
 * in the scheduler's headers. Bit i set means "CPU i might hold a stale
 * translation for this space"; returning ~0ULL (every bit set) is always a
 * safe answer, just an unnecessarily broad one. */
extern u64 sched_tlb_current_space_mask(phys_addr_t space);

/* Companion bridge, same module boundary and same restriction: the PCID tag
 * @space's translations are cached under, or 0 when this CPU cannot say. */
extern u16 sched_tlb_space_pcid(phys_addr_t space);

/* Shared core of tlb_shootdown_all()/_space()/_user(): queue the request on
 * every CPU in @cpu_mask (other than this one), IPI them, and — if it is safe
 * to wait — block until each has acknowledged.
 *
 * @pcid is the one address space that needs invalidating, or 0 for "everything"
 * (which is what the all- and space-variants ask for). */
static void tlb_shootdown_mask(u64 cpu_mask, u16 pcid)
{
    u32 n = smp_cpu_count();
    if (n <= 1) return;
    if (n > SMP_MAX_CPUS) n = SMP_MAX_CPUS;

    u32 me = smp_current_cpu_id();
    u64 ticket[SMP_MAX_CPUS];
    u64 sent = 0; /* set of CPUs we actually IPI'd, for the wait loop below */

    /* A CPU that has not finished bringing itself up has no IDT loaded and
     * will not run the shootdown handler, so an IPI to it is dropped on the
     * floor — but the wait loop below would still block on an acknowledgement
     * that can never arrive. Restricting the target set to CPUs that are
     * actually online is what keeps a shootdown issued during AP bring-up (and
     * driver init issues plenty) from hanging the boot. */
    cpu_mask &= smp_online_mask();

    for (u32 i = 0; i < n; i++) {
        if (i == me) continue;
        if (!((cpu_mask >> i) & 1ULL)) continue;

        /* Merge the scope in, then take the ticket — both under the target's
         * lock, so its handler can never acknowledge a ticket whose scope it
         * has not yet seen. The IPI goes out after every ticket is taken. */
        irqflags_t qf = spinlock_lock_irqsave(&g_tlb_cpu[i].lock);
        tlb_queue_pcid(&g_tlb_cpu[i], pcid);
        ticket[i] = __atomic_add_fetch(&g_tlb_cpu[i].req, 1, __ATOMIC_SEQ_CST);
        spinlock_unlock_irqrestore(&g_tlb_cpu[i].lock, qf);

        sent |= (1ULL << i);
    }

    if (!sent) return;

    /* One send for the whole target set. smp_send_ipi_mask() collapses to the
     * local APIC's all-excluding-self shorthand when the set is every other
     * online CPU — which is the common case here, since most shootdowns are
     * global — turning what used to be one ICR write per CPU into one write
     * total. On a 64-thread machine that is 63 uncached MMIO stores saved on
     * every unmap, with every one of them serialised behind the delivery of
     * the last under xAPIC. */
    smp_send_ipi_mask(sent, TLB_SHOOTDOWN_VECTOR);

    /* See the header: waiting is only safe while we can still service someone
     * else's shootdown ourselves. */
    if (!irqs_enabled()) return;

    /* Bounded per-CPU wait, with the IPI re-sent if a target hasn't
     * acknowledged within one budget — confirmed live (gdb attached to a
     * stuck boot, see the commit this belongs to) that the plain "spin
     * forever" version below can wait on a target that is verifiably
     * idle, interrupts enabled, ticking normally, and simply never runs
     * tlb_shootdown_ipi() for that request: some race between this send
     * and the target's LAPIC (most plausible: the target still had our
     * vector in-service from a request moments earlier when this second
     * one arrived, and whatever should re-raise it once that finishes
     * didn't) drops the interrupt rather than queuing it. However that
     * happens, re-sending it is always a safe recovery: smp_send_ipi() is
     * a fresh, independent delivery attempt, and the target's handler is
     * idempotent (it always resolves to "flush, then publish whatever
     * req currently reads," never "the specific request that triggered
     * this delivery") so a resend can only ever produce the same
     * outcome as this one arriving cleanly the first time — there is no
     * double-flush hazard to worry about. */
    for (u32 i = 0; i < n; i++) {
        if (!((sent >> i) & 1ULL)) continue;
        u64 wait_start = rdtsc();
        u64 budget     = TLB_SHOOTDOWN_FIRST_RESEND_CYCLES;
        u32 resends = 0;
        u32 spins   = 0;
        while (__atomic_load_n(&g_tlb_cpu[i].done, __ATOMIC_SEQ_CST) < ticket[i]) {
            /* Same backoff discipline the ticket spinlock uses: plain PAUSE for
             * the first rounds, then let hw_spin_wait() park the core in
             * TPAUSE's C0.2 state where the CPU has it. A shootdown wait is
             * exactly the case that benefits — the answer comes from another
             * core, so spinning at full rate buys nothing and starves the SMT
             * sibling of a pipeline it could be using. */
            hw_spin_wait(spins++);
            if (rdtsc() - wait_start < budget) continue;
            wait_start = rdtsc();
            budget     = TLB_SHOOTDOWN_RESEND_CYCLES;
            resends++;
            /* The first resend is routine recovery on the millisecond budget
             * above, not a symptom, so it stays quiet; anything past that has
             * survived a full long budget and is worth reporting. */
            if (resends == 2 || resends % 16 == 0) {
                kprintf("[TLB] shootdown to CPU%u stuck after %u resend(s) "
                        "(req=%llu, done=%llu) — resending IPI\n",
                        i, resends,
                        (unsigned long long)ticket[i],
                        (unsigned long long)__atomic_load_n(&g_tlb_cpu[i].done, __ATOMIC_RELAXED));
            }
            smp_send_ipi(i, TLB_SHOOTDOWN_VECTOR);
        }
    }
}

void tlb_shootdown_all(void)
{
    tlb_shootdown_mask(~0ULL, 0);
}

void tlb_shootdown_space(phys_addr_t space)
{
    /* Bail out *before* calling sched_tlb_current_space_mask(): with only one
     * CPU up there is nothing to notify regardless of the mask, and this
     * matters beyond the obvious short-circuit — vmm_set_flags()'s own NX-bit
     * setup runs during early boot, single-CPU, before per-CPU/scheduler
     * state exists at all, and used to reach this point through plain
     * tlb_shootdown_all() (which has this exact same early-out) without ever
     * touching that state. sched_tlb_current_space_mask() reads the current
     * CPU's running thread via smp_get_cpu(), which is only meaningful once
     * a second CPU could plausibly be up (i.e. scheduler/SMP bring-up has
     * happened) — calling it any earlier than that is exactly what caused a
     * boot-time double fault the first time this function existed. */
    if (smp_cpu_count() <= 1) return;
    tlb_shootdown_mask(sched_tlb_current_space_mask(space), 0);
}

void tlb_shootdown_user(phys_addr_t space, virt_addr_t start, size_t npages)
{
    /* Same early-out, and for the same reason, as tlb_shootdown_space(). */
    if (smp_cpu_count() <= 1) return;

    u16 pcid = 0;   /* 0 => could not narrow it; targets flush everything */

    /* Every condition in tlb.h's contract, checked here so the handler has
     * nothing left to decide. The npages bound is what keeps the end-of-range
     * arithmetic below from wrapping on a nonsense count. */
    if (npages != 0 && npages <= (TLB_USER_VA_LIMIT / PAGE_SIZE) &&
        start < TLB_USER_VA_LIMIT &&
        (start + npages * PAGE_SIZE) <= TLB_USER_VA_LIMIT &&
        g_pcid_enabled && g_invpcid_enabled) {
        pcid = sched_tlb_space_pcid(space);
    }

    tlb_shootdown_mask(sched_tlb_current_space_mask(space), pcid);
}

void tlb_shootdown_ipi(void)
{
    u32 me = smp_current_cpu_id();
    if (me >= SMP_MAX_CPUS) return;
    tlb_cpu_state_t *st = &g_tlb_cpu[me];

    /* Snapshot the counter and the scope together, under the lock the
     * initiators bump `req` beneath. That pairing is what makes the
     * acknowledgement honest: everything counted in `seen` has already been
     * merged into the descriptor this reads, so the flush below covers it.
     * Anything requested after this point lands in a fresh descriptor and a
     * higher ticket, and is not reported as complete. */
    /*
     * Loop until this CPU has acknowledged everything that was asked of it,
     * rather than handling one delivery and returning.
     *
     * The LAPIC will not queue a second delivery of a vector that is already in
     * service. An initiator that bumps `req` while this handler is running can
     * therefore have its IPI dropped outright — the case the wait loop's resend
     * logic above was written for, and what the "[TLB] shootdown to CPUn stuck"
     * warning reports when it fires. Recovery then costs a full resend budget
     * of spinning on the initiator, which is deliberately long.
     *
     * None of that needs an interrupt to resolve, because none of it needs to
     * be *told*. `req` is the whole truth about what is outstanding and this
     * CPU can read it directly, so re-checking it after publishing `done`
     * converts a dropped delivery into one more iteration here. The work gets
     * done because it was requested, not because a second interrupt happened to
     * arrive; the resend path stays as a last resort for a delivery lost some
     * other way.
     *
     * It terminates for the same reason the initiator's wait does: `req` only
     * advances when some CPU adds work, and every iteration flushes whatever
     * had accumulated at the moment it looked.
     */
    for (;;) {
        irqflags_t f = spinlock_lock_irqsave(&st->lock);
        u64  seen      = __atomic_load_n(&st->req, __ATOMIC_SEQ_CST);
        bool flush_all = st->flush_all;
        u16  pcid      = st->pcid;
        st->flush_all  = false;
        st->pcid       = 0;
        spinlock_unlock_irqrestore(&st->lock, f);

        /* The initiator already established that a nonzero pcid means "a user
         * range in a user address space, on a CPU with PCID and INVPCID" — see
         * tlb_shootdown_user(). Single-context invalidation then drops exactly
         * that space's translations and paging-structure entries, on this core,
         * without touching CR3 and without discarding the global kernel entries
         * that a full flush would have thrown away. */
        if (!flush_all && pcid != 0)
            invpcid(INVPCID_SINGLE_CTX, pcid, 0);
        else
            tlb_flush_local_global();

        __atomic_store_n(&st->done, seen, __ATOMIC_SEQ_CST);

        /* Nothing new arrived while we were flushing: every outstanding ticket
         * is now acknowledged and any further request will deliver its own
         * interrupt. */
        if (__atomic_load_n(&st->req, __ATOMIC_SEQ_CST) == seen) return;
    }
}
