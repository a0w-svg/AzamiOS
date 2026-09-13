/* ============================================================================
 * AzamiOS — Cross-CPU TLB Shootdown (x86_64)
 * File: arch/x86_64/mm/tlb.c
 * ============================================================================ */

#include "tlb.h"
#include "../cpu/msr.h"
#include "../cpu/cpu.h"
#include "../cpu/smp.h"
#include "../cpu/lapic.h"
#include "../../../include/azami/defs.h"
#include "../../../drivers/char/console.h"

#define TLB_SHOOTDOWN_VECTOR  251

/* Spin iterations (each one a cpu_pause()) before concluding a target's
 * ack is late enough to be worth re-sending the IPI rather than just
 * still in flight. Not a real-time bound — cpu_pause() cost varies a lot
 * by host — just short enough that a genuinely dropped IPI (see the wait
 * loop's comment) gets retried in well under a second instead of spinning
 * unbounded. */
#define TLB_SHOOTDOWN_RESEND_BUDGET  2000000u

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
    u8           _pad[48]; /* Pad to 64-byte cache line boundary */
} __attribute__((aligned(64))) tlb_cpu_state_t;

static tlb_cpu_state_t g_tlb_cpu[SMP_MAX_CPUS];

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

/* Shared core of tlb_shootdown_all()/tlb_shootdown_space(): IPI every CPU in
 * @cpu_mask (other than this one) and, if it is safe to wait, block until
 * each has acknowledged. */
static void tlb_shootdown_mask(u64 cpu_mask)
{
    u32 n = smp_cpu_count();
    if (n <= 1) return;
    if (n > SMP_MAX_CPUS) n = SMP_MAX_CPUS;

    u32 me = smp_current_cpu_id();
    u64 ticket[SMP_MAX_CPUS];
    u64 sent = 0; /* set of CPUs we actually IPI'd, for the wait loop below */

    for (u32 i = 0; i < n; i++) {
        if (i == me) continue;
        if (!((cpu_mask >> i) & 1ULL)) continue;
        ticket[i] = __atomic_add_fetch(&g_tlb_cpu[i].req, 1, __ATOMIC_SEQ_CST);
        smp_send_ipi(i, TLB_SHOOTDOWN_VECTOR);
        sent |= (1ULL << i);
    }

    if (!sent) return;

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
        u32 spins = 0;
        u32 resends = 0;
        while (__atomic_load_n(&g_tlb_cpu[i].done, __ATOMIC_SEQ_CST) < ticket[i]) {
            cpu_pause();
            if (++spins < TLB_SHOOTDOWN_RESEND_BUDGET) continue;
            spins = 0;
            resends++;
            if (resends == 1 || resends % 16 == 0) {
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
    tlb_shootdown_mask(~0ULL);
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
    tlb_shootdown_mask(sched_tlb_current_space_mask(space));
}

void tlb_shootdown_ipi(void)
{
    u32 me = smp_current_cpu_id();
    if (me >= SMP_MAX_CPUS) return;

    /* Snapshot first: anything requested after this point is not covered by the
     * flush we are about to do, and must not be reported as complete. */
    u64 seen = __atomic_load_n(&g_tlb_cpu[me].req, __ATOMIC_SEQ_CST);
    tlb_flush_local_global();
    __atomic_store_n(&g_tlb_cpu[me].done, seen, __ATOMIC_SEQ_CST);
}
