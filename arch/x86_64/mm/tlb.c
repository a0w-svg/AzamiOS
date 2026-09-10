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

#define TLB_SHOOTDOWN_VECTOR  251

/* Request/completion counters, one pair per CPU.
 *
 * An initiator bumps g_tlb_req[target] and remembers the value it produced;
 * the target snapshots g_tlb_req[self] *before* flushing and stores that
 * snapshot into g_tlb_done[self] afterwards. So g_tlb_done[i] >= my_ticket
 * proves a flush that *started* after my page-table edit has finished — which
 * is the property the caller actually needs, and it holds even when several
 * CPUs shoot down at once or a stray IPI arrives. No lock is involved, so the
 * handler can never be blocked by whoever is waiting on it. */
static volatile u64 g_tlb_req[SMP_MAX_CPUS];
static volatile u64 g_tlb_done[SMP_MAX_CPUS];

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

void tlb_shootdown_all(void)
{
    u32 n = smp_cpu_count();
    if (n <= 1) return;
    if (n > SMP_MAX_CPUS) n = SMP_MAX_CPUS;

    u32 me = smp_current_cpu_id();
    u64 ticket[SMP_MAX_CPUS];

    for (u32 i = 0; i < n; i++) {
        if (i == me) continue;
        ticket[i] = __atomic_add_fetch(&g_tlb_req[i], 1, __ATOMIC_SEQ_CST);
        smp_send_ipi(i, TLB_SHOOTDOWN_VECTOR);
    }

    /* See the header: waiting is only safe while we can still service someone
     * else's shootdown ourselves. */
    if (!irqs_enabled()) return;

    for (u32 i = 0; i < n; i++) {
        if (i == me) continue;
        while (__atomic_load_n(&g_tlb_done[i], __ATOMIC_SEQ_CST) < ticket[i])
            cpu_pause();
    }
}

void tlb_shootdown_ipi(void)
{
    u32 me = smp_current_cpu_id();
    if (me >= SMP_MAX_CPUS) return;

    /* Snapshot first: anything requested after this point is not covered by the
     * flush we are about to do, and must not be reported as complete. */
    u64 seen = __atomic_load_n(&g_tlb_req[me], __ATOMIC_SEQ_CST);
    tlb_flush_local_global();
    __atomic_store_n(&g_tlb_done[me], seen, __ATOMIC_SEQ_CST);
}
