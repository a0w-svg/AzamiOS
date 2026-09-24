/* ============================================================================
 * AzamiOS — Local APIC Driver Implementation (xAPIC / x2APIC)
 * File: arch/x86_64/cpu/lapic.c
 * ============================================================================ */

#include "lapic.h"
#include "cpu.h"
#include "msr.h"
#include "pic.h"
#include "idt.h"
#include "../mm/vmm.h"
#include "../../../drivers/char/console.h"
#include "../../../drivers/misc/hpet.h"
#include "../../../include/azami/defs.h"

/* LAPIC virtual address via HHDM (xAPIC mode only; unused under x2APIC). */
static volatile u32 *g_lapic_mmio = NULL;

/* Set once, on the BSP, by lapic_init(). Every CPU then follows the same
 * decision: mixing modes across cores would mean IPI destinations encoded one
 * way being decoded another. Read on every register access, so it is a plain
 * bool rather than an atomic — it is written before any AP exists. */
static bool g_x2apic = false;

/* Calibrated in lapic_timer_calibrate() on the BSP. The APIC bus clock and the
 * TSC both run at a rate uniform across the cores of a package, so one
 * measurement serves every CPU. */
static u32 g_lapic_ticks_per_ms = 10000;   /* fallback until calibrated */
static u32 g_tsc_khz = 0;                  /* 0 = TSC-deadline unavailable */

/* Non-zero once lapic_timer_start() has chosen TSC-deadline mode: the TSC
 * delta between two scheduler ticks. Per-CPU state is not needed — every CPU
 * arms the same frequency — but the *deadline* is, and that lives in the
 * IA32_TSC_DEADLINE MSR, which is already per-CPU hardware. */
static u64 g_tsc_deadline_period = 0;

/* ── Register access ──────────────────────────────────────────────────────── */

static inline void lapic_write(u32 reg, u32 val)
{
    if (g_x2apic) {
        wrmsr(MSR_X2APIC_BASE + (reg >> 4), (u64)val);
    } else if (g_lapic_mmio) {
        g_lapic_mmio[reg / 4] = val;
    }
}

static inline u32 lapic_read(u32 reg)
{
    if (g_x2apic) return (u32)rdmsr(MSR_X2APIC_BASE + (reg >> 4));
    if (g_lapic_mmio) return g_lapic_mmio[reg / 4];
    return 0;
}

bool lapic_x2apic_active(void)
{
    return g_x2apic;
}

phys_addr_t lapic_base_phys(void)
{
    u64 base = rdmsr(MSR_APIC_BASE);
    return (phys_addr_t)(base & 0xFFFFFFFFFFFFF000ULL);
}

u32 lapic_id(void)
{
    if (g_x2apic) return (u32)rdmsr(MSR_X2APIC_BASE + (LAPIC_ID >> 4));
    if (!g_lapic_mmio) return 0;
    return (g_lapic_mmio[LAPIC_ID / 4] >> 24) & 0xFF;
}

u32 lapic_error_status(void)
{
    /* The ESR latches errors only when written first — an architectural quirk
     * that makes a bare read return the previous snapshot rather than the
     * current state. Write-then-read is the documented sequence. */
    lapic_write(LAPIC_ESR, 0);
    u32 esr = lapic_read(LAPIC_ESR);
    lapic_write(LAPIC_ESR, 0);
    return esr;
}

/* ── Initialisation ───────────────────────────────────────────────────────── */

/*
 * Transition this CPU into x2APIC mode.
 *
 * The architecture only allows disabled -> xAPIC -> x2APIC; going straight
 * from disabled to x2APIC, or back from x2APIC to xAPIC, raises #GP. So the
 * sequence is: make sure EN is set (xAPIC), then set EXTD alongside it.
 *
 * Once EXTD is set, the MMIO window is gone: further stores to it are
 * architecturally undefined, which is why lapic_write() dispatches on g_x2apic
 * rather than writing both.
 */
static bool lapic_enable_x2apic(void)
{
    u64 base = rdmsr(MSR_APIC_BASE);

    if (base & APIC_BASE_EXTD) return true;   /* firmware already did it */

    /* Step 1: xAPIC enabled. */
    if (!(base & APIC_BASE_ENABLE)) {
        base |= APIC_BASE_ENABLE;
        wrmsr(MSR_APIC_BASE, base);
    }

    /* Step 2: xAPIC -> x2APIC. */
    wrmsr(MSR_APIC_BASE, base | APIC_BASE_ENABLE | APIC_BASE_EXTD);

    base = rdmsr(MSR_APIC_BASE);
    return (base & APIC_BASE_EXTD) != 0;
}

void lapic_init(void)
{
    bool is_bsp = (rdmsr(MSR_APIC_BASE) & APIC_BASE_BSP) != 0;

    /* The BSP decides the mode for the whole machine; APs follow it. An AP
     * that found x2APIC unusable while the BSP is using it would be
     * unaddressable, so treat a failed transition on an AP as fatal rather
     * than silently leaving one core behind. */
    if (is_bsp) {
        if (g_cpu_info.has_x2apic && lapic_enable_x2apic()) {
            g_x2apic = true;
        }
    } else if (g_x2apic) {
        if (!lapic_enable_x2apic())
            PANIC("LAPIC: AP failed to enter x2APIC mode the BSP is using");
    }

    if (!g_x2apic) {
        /* xAPIC: enable in MSR_APIC_BASE and map the MMIO page once. */
        u64 base = rdmsr(MSR_APIC_BASE);
        base |= APIC_BASE_ENABLE;
        wrmsr(MSR_APIC_BASE, base);

        phys_addr_t phys = lapic_base_phys();
        g_lapic_mmio = (volatile u32 *)PHYS_TO_VIRT(phys);

        /* Every CPU sees the same physical LAPIC page at the same address;
         * mapping it again from an AP is idempotent. */
        vmm_map(vmm_kernel_space(), (virt_addr_t)g_lapic_mmio, phys, VMM_MMIO);

        /* Flat logical destination model with this CPU's own bit set, so
         * logical-mode IPIs (and the IO-APIC's lowest-priority routing) have
         * a coherent view. The flat model tops out at 8 CPUs; beyond that
         * xAPIC callers use physical destinations, which lapic_send_ipi()
         * does unconditionally. */
        lapic_write(LAPIC_DFR, 0xFFFFFFFFU);            /* flat model */
        u32 id = (g_lapic_mmio[LAPIC_ID / 4] >> 24) & 0xFF;
        u32 ldr = lapic_read(LAPIC_LDR) & 0x00FFFFFFU;
        lapic_write(LAPIC_LDR, ldr | ((1U << (id & 7)) << 24));
    }

    /* Enable APIC via SVR and map spurious vector to 255. */
    lapic_write(LAPIC_SVR, LAPIC_SVR_ENABLE | LAPIC_SVR_SPURIOUS);

    /* Clear TPR so all priority interrupts can be received. */
    lapic_write(LAPIC_TPR, 0);

    /* Mask the LVT sources nothing handles yet, so a thermal event or a
     * performance-counter overflow cannot deliver a vector no handler owns.
     * LINT0/LINT1 are left to the BSP's firmware configuration on the BSP
     * (8259 pass-through and the NMI line) and masked on APs, which is what
     * the MP spec's default configuration describes. */
    lapic_write(LAPIC_LVT_THERMAL, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_PERF,    LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_ERROR,   LAPIC_LVT_MASKED);
    if (!is_bsp) {
        lapic_write(LAPIC_LVT_LINT0, LAPIC_LVT_MASKED);
        lapic_write(LAPIC_LVT_LINT1, LAPIC_LVT_MASKED);
    }

    /* Acknowledge any outstanding interrupt and clear latched errors. */
    lapic_write(LAPIC_EOI, 0);
    lapic_error_status();

    if (is_bsp) {
        kprintf("[LAPIC] %s mode enabled (ID=%u, version=0x%02x)\n",
                g_x2apic ? "x2APIC" : "xAPIC",
                lapic_id(), lapic_read(LAPIC_VERSION) & 0xFF);
    }
}

void lapic_eoi(void)
{
    lapic_write(LAPIC_EOI, 0);
}

/* ── Timer ────────────────────────────────────────────────────────────────── */

/* g_lapic_ticks_per_ms starts as a guess (10000) because the LAPIC timer runs
 * off the bus/APIC clock, whose real frequency varies by platform and is not
 * knowable from CPUID. lapic_timer_start(hz) uses it to convert a requested
 * frequency into a tick count, and this kernel's own scheduler assumes the
 * resulting periodic rate is exactly what it asked for -- TCP/ARP/IP timers
 * gate off "every 100th tick == 1 second" and dethread teardown computes a
 * 5-second deadline the same way. An uncalibrated guess makes all of those
 * real-world durations wrong by whatever the guess is off by. HPET (already
 * initialised in kernel_main() before this runs) gives a real nanosecond time
 * source to measure both counters against, so there is no need to guess when
 * hardware can just be asked.
 *
 * The TSC is measured in the same window. It costs nothing extra (one RDTSC
 * at each end of a wait that is already happening) and it is what unlocks
 * TSC-deadline timer mode below. */
void lapic_timer_calibrate(void)
{
    if (!hpet_available()) return;

    lapic_write(LAPIC_TIMER_DIV, LAPIC_TIMER_DIV_16);
    lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED | LAPIC_TIMER_ONESHOT);
    lapic_write(LAPIC_TIMER_INIT, 0xFFFFFFFFU);

    u64 start_ns  = hpet_now_ns();
    u64 start_tsc = rdtsc_ordered();
    const u64 window_ns = 10000000ULL; /* 10ms: long enough for good precision,
                                        * short enough not to wrap the 32-bit
                                        * counter on any real bus clock. */
    while (hpet_now_ns() - start_ns < window_ns) {
        cpu_pause();
    }
    u32 curr       = lapic_read(LAPIC_TIMER_CURR);
    u64 end_tsc    = rdtsc_ordered();
    u64 elapsed_ns = hpet_now_ns() - start_ns;

    lapic_write(LAPIC_TIMER_INIT, 0); /* stop counting */

    if (elapsed_ns == 0) return;      /* keep the fallback guess */

    if (curr < 0xFFFFFFFFU) {
        u64 elapsed_ticks = 0xFFFFFFFFULL - curr;
        u32 ticks_per_ms  = (u32)((elapsed_ticks * 1000000ULL) / elapsed_ns);
        if (ticks_per_ms) {
            g_lapic_ticks_per_ms = ticks_per_ms;
            kprintf("[LAPIC] Timer calibrated against HPET: %u ticks/ms (div-by-16)\n",
                    g_lapic_ticks_per_ms);
        }
    }

    u64 tsc_delta = end_tsc - start_tsc;
    if (tsc_delta) {
        u32 khz = (u32)((tsc_delta * 1000000ULL) / elapsed_ns);
        if (khz) {
            g_tsc_khz = khz;
            /* Publish it for everything else that wants a wall-clock TSC:
             * CPUID leaf 0x15/0x16 is absent on plenty of parts (and on QEMU
             * without +invtsc), and a measured value beats no value. */
            if (g_cpu_info.tsc_khz == 0) g_cpu_info.tsc_khz = khz;
            kprintf("[LAPIC] TSC calibrated against HPET: %u.%03u MHz\n",
                    khz / 1000, khz % 1000);
        }
    }
}

u32 lapic_tsc_khz(void)
{
    return g_tsc_khz ? g_tsc_khz : g_cpu_info.tsc_khz;
}

/*
 * TSC-deadline mode is preferred wherever the CPU offers it, for the same
 * reasons Linux prefers it:
 *
 *   - The deadline is an absolute TSC value written to one MSR, so arming the
 *     next tick is a single WRMSR with no divider arithmetic and no MMIO.
 *   - It does not drift with the APIC bus clock, and it keeps running in the
 *     deeper C-states where the LAPIC counter stops unless ARAT is present.
 *   - Resolution is a TSC tick rather than a bus tick, which is what a future
 *     tickless/high-resolution timer path needs.
 *
 * It is one-shot by construction, so the vector-48 handler must re-arm it:
 * see lapic_timer_rearm(), called from sched_tick().
 */
static bool lapic_use_tsc_deadline(void)
{
    return g_cpu_info.has_tsc_deadline && lapic_tsc_khz() != 0;
}

void lapic_timer_start(u32 hz)
{
    if (hz == 0) return;

    if (lapic_use_tsc_deadline()) {
        u64 khz = lapic_tsc_khz();
        g_tsc_deadline_period = (khz * 1000ULL) / hz;
        if (g_tsc_deadline_period == 0) g_tsc_deadline_period = 1;

        lapic_write(LAPIC_LVT_TIMER, LAPIC_TIMER_TSCDEADL | LAPIC_TIMER_VECTOR);
        /* An LVT write and the first deadline write must not be reordered
         * across each other: the MSR is only honoured once the LVT selects
         * deadline mode. MFENCE is the architecturally required separator. */
        __asm__ volatile("mfence" ::: "memory");
        wrmsr(MSR_IA32_TSC_DEADLINE, rdtsc() + g_tsc_deadline_period);
        return;
    }

    u32 count = (g_lapic_ticks_per_ms * 1000) / hz;
    if (count == 0) count = 100;

    g_tsc_deadline_period = 0;
    lapic_write(LAPIC_TIMER_DIV, LAPIC_TIMER_DIV_16);
    lapic_write(LAPIC_LVT_TIMER, LAPIC_TIMER_PERIODIC | LAPIC_TIMER_VECTOR);
    lapic_write(LAPIC_TIMER_INIT, count);
}

void lapic_timer_rearm(void)
{
    if (!g_tsc_deadline_period) return;   /* periodic mode re-arms itself */

    /* Advance from *now* rather than from the previous deadline. Chaining
     * deadlines would be more accurate in the abstract, but a tick handler
     * that overran its period even once would then have every subsequent
     * deadline already in the past, and the CPU would deliver ticks
     * back-to-back trying to catch up — a livelock the periodic mode this
     * replaces could not produce. Linux's clockevents core makes the same
     * trade for exactly this reason. */
    wrmsr(MSR_IA32_TSC_DEADLINE, rdtsc() + g_tsc_deadline_period);
}

void lapic_timer_stop(void)
{
    if (g_tsc_deadline_period) {
        wrmsr(MSR_IA32_TSC_DEADLINE, 0);
    } else {
        lapic_write(LAPIC_TIMER_INIT, 0);
    }
    lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED);
}

/* ── Inter-processor interrupts ───────────────────────────────────────────── */

/* The xAPIC ICR hi/lo pair is this core's own local-APIC state, written as two
 * separate MMIO stores with no hardware atomicity between them. A vector-48
 * (or any other) interrupt landing on *this* core between the two stores runs
 * with interrupts enabled at the call site, and if its handler also sends an
 * IPI (sched_tick()'s wakeup path calls smp_send_reschedule() from exactly
 * this context), that nested call clobbers ICR_HI with its own destination
 * before the outer call's ICR_LO write fires — so the outer IPI goes out with
 * the wrong destination and the intended target never sees it. Nothing polls
 * for that failure (there is nothing to poll: the send *looks* successful),
 * so the caller silently waits forever, which is what tlb_shootdown_mask()'s
 * "stuck after N resends" case was actually seeing on real hardware, where
 * genuine interrupt preemption between two MMIO stores is routine — TCG
 * emulation never reproduced it because it doesn't preempt mid-callout the
 * same way. Local interrupts are the only possible source of reentrancy here
 * (each CPU owns its own physical LAPIC), so disabling them around the
 * wait+HI+LO sequence is sufficient; no cross-CPU lock is needed.
 *
 * x2APIC does not have the problem at all: its ICR is one 64-bit MSR and a
 * single WRMSR carries destination and command together, which is why the
 * x2APIC path below takes no guard. */
static inline u64 lapic_icr_guard_enter(void)
{
    u64 flags;
    __asm__ volatile(
        "pushfq         \n"
        "popq  %0       \n"
        "cli            \n"
        : "=r"(flags) : : "memory"
    );
    return flags;
}

static inline void lapic_icr_guard_exit(u64 flags)
{
    __asm__ volatile(
        "pushq %0   \n"
        "popfq      \n"
        : : "r"(flags) : "memory"
    );
}

/* One send, both generations. @dest is ignored for shorthand deliveries. */
static void lapic_icr_send(u32 dest, u32 icr_lo)
{
    if (g_x2apic) {
        /* Bits 63:32 are the full 32-bit destination; the delivery-status bit
         * does not exist (WRMSR itself blocks until the send is accepted). */
        wrmsr(MSR_X2APIC_ICR, ((u64)dest << 32) | (u64)icr_lo);
        return;
    }

    if (!g_lapic_mmio) return;

    u64 f = lapic_icr_guard_enter();
    while (lapic_read(LAPIC_ICR_LO) & LAPIC_ICR_BUSY) cpu_pause();
    lapic_write(LAPIC_ICR_HI, (dest & 0xFFU) << 24);
    lapic_write(LAPIC_ICR_LO, icr_lo);
    lapic_icr_guard_exit(f);
}

void lapic_send_ipi(u32 apic_id, u8 vector)
{
    lapic_icr_send(apic_id, LAPIC_ICR_FIXED | LAPIC_ICR_ASSERT | vector);
}

void lapic_send_ipi_allbutself(u8 vector)
{
    lapic_icr_send(0, LAPIC_ICR_FIXED | LAPIC_ICR_ASSERT |
                      LAPIC_ICR_DSH_ALLBUT | vector);
}

void lapic_send_ipi_self(u8 vector)
{
    if (g_x2apic) {
        /* The dedicated self-IPI MSR skips the ICR entirely and cannot be
         * reordered against another core's send. */
        wrmsr(MSR_X2APIC_SELF_IPI, vector);
        return;
    }
    lapic_icr_send(0, LAPIC_ICR_FIXED | LAPIC_ICR_ASSERT |
                      LAPIC_ICR_DSH_SELF | vector);
}

void lapic_send_nmi(u32 apic_id)
{
    lapic_icr_send(apic_id, LAPIC_ICR_NMI | LAPIC_ICR_ASSERT);
}

void lapic_send_nmi_allbutself(void)
{
    lapic_icr_send(0, LAPIC_ICR_NMI | LAPIC_ICR_ASSERT | LAPIC_ICR_DSH_ALLBUT);
}

void lapic_send_init(u32 apic_id)
{
    lapic_icr_send(apic_id, LAPIC_ICR_INIT | LAPIC_ICR_LEVEL | LAPIC_ICR_ASSERT);
}

void lapic_send_sipi(u32 apic_id, u8 trampoline_page)
{
    lapic_icr_send(apic_id, LAPIC_ICR_SIPI | LAPIC_ICR_ASSERT | trampoline_page);
}
