/* ============================================================================
 * AzamiOS — Symmetric Multiprocessing (SMP) Implementation
 * File: arch/x86_64/cpu/smp.c
 * ============================================================================ */

#include "smp.h"
#include "cpu.h"
#include "gdt.h"
#include "idt.h"
#include "lapic.h"
#include "msr.h"
#include "spinlock.h"
#include "topology.h"
#include "hwaccel.h"   /* hw_popcnt64 / hw_ctz64 */
#include "../boot/limine_req.h"
#include "../mm/vmm.h"
#include "../../../kernel/mm/pmm.h"
#include "../../../drivers/char/console.h"
#include "../../../include/azami/defs.h"

static cpu_info_t g_cpu_infos[SMP_MAX_CPUS];
static u32 g_cpu_count = 1;
static volatile u32 g_aps_online = 0;

/* Bit k set once CPU k has finished bringing itself up and is answering IPIs.
 * Every cross-CPU send consults this: an IPI to a CPU that has not reached
 * lapic_init() yet is delivered to hardware that is not listening, and the
 * sender then waits for an acknowledgement that can never come. */
static volatile u64 g_cpu_online_mask = 0;

/* Per-CPU kernel stack size: 16 KB (4 pages) */
#define KERNEL_STACK_PAGES  4
#define KERNEL_STACK_SIZE   (KERNEL_STACK_PAGES * PAGE_SIZE)

extern volatile struct limine_smp_request g_limine_smp_req;

u32 smp_cpu_count(void)
{
    return g_cpu_count;
}

cpu_info_t *smp_cpu_info(u32 cpu_id)
{
    if (cpu_id >= SMP_MAX_CPUS) return NULL;
    return &g_cpu_infos[cpu_id];
}

u64 smp_online_mask(void)
{
    return __atomic_load_n(&g_cpu_online_mask, __ATOMIC_ACQUIRE);
}

bool smp_cpu_online(u32 cpu_id)
{
    if (cpu_id >= SMP_MAX_CPUS) return false;
    return (smp_online_mask() & (1ULL << cpu_id)) != 0;
}

u32 smp_cpu_apic_id(u32 cpu_id)
{
    if (cpu_id >= SMP_MAX_CPUS) return 0;
    return g_cpu_infos[cpu_id].lapic_id;
}

volatile bool g_smp_sched_active = false;

/* ── Cross-CPU function calls ─────────────────────────────────────────────── */

/*
 * One request. The sender owns the storage — it lives on the sender's stack
 * for the duration of the call — and the target only ever reads it and then
 * publishes `done`. Nothing is allocated, which is what lets this be called
 * from contexts where allocation is not possible (interrupt handlers, the
 * panic path, memory-manager internals).
 */
struct smp_csd {
    struct smp_csd *next;
    void          (*func)(void *info);
    void           *info;
    volatile u32    done;
};

/* Per-CPU inbox, pushed by any sender and drained wholesale by the owner.
 * A lock-free LIFO: push is one compare-exchange, drain is one exchange. No
 * lock is taken on either side, which matters because the drain runs in
 * interrupt context and a sender may be holding arbitrary locks. */
static struct smp_csd * volatile g_call_queue[SMP_MAX_CPUS];

/* Execute (and acknowledge) everything queued for the calling CPU. */
static void smp_call_drain_local(void)
{
    u32 me = smp_current_cpu_id();
    if (me >= SMP_MAX_CPUS) return;

    struct smp_csd *list = __atomic_exchange_n(&g_call_queue[me], NULL,
                                               __ATOMIC_ACQUIRE);
    while (list) {
        /* Read `next` before running the callback: once `done` is published
         * the sender may return, and the csd storage (its stack frame) can be
         * reused out from under us. */
        struct smp_csd *next = list->next;
        void (*func)(void *) = list->func;
        void *info = list->info;

        if (func) func(info);

        __atomic_store_n(&list->done, 1, __ATOMIC_RELEASE);
        list = next;
    }
}

void smp_call_function_interrupt(void)
{
    cpu_info_t *cpu = smp_get_cpu();
    if (cpu) cpu->ipis_call++;
    smp_call_drain_local();
}

static void smp_call_queue_push(u32 target, struct smp_csd *csd)
{
    struct smp_csd *old = __atomic_load_n(&g_call_queue[target], __ATOMIC_RELAXED);
    do {
        csd->next = old;
    } while (!__atomic_compare_exchange_n(&g_call_queue[target], &old, csd,
                                          true, __ATOMIC_RELEASE, __ATOMIC_RELAXED));
}

/*
 * Wait for a set of requests to be acknowledged.
 *
 * The loop drains this CPU's own inbox on every pass. That is not an
 * optimisation, it is what makes the primitive safe: if CPU A calls into B at
 * the same moment B calls into A, and each only spun on its own request, both
 * would wait forever for a handler the other will never get round to running.
 * Interrupts may also legitimately be disabled at the call site (the TLB and
 * panic paths both do it), in which case the SMP_VEC_CALL_FUNC interrupt will
 * never fire here and draining by hand is the only way the work gets done.
 */
static void smp_call_wait(struct smp_csd *csds, u64 pending_mask)
{
    u64 remaining = pending_mask;
    u64 spins = 0;

    while (remaining) {
        smp_call_drain_local();

        u64 still = 0;
        u64 m = remaining;
        while (m) {
            u32 c = hw_ctz64(m);
            m &= m - 1;
            if (!__atomic_load_n(&csds[c].done, __ATOMIC_ACQUIRE))
                still |= (1ULL << c);
        }
        remaining = still;
        if (!remaining) break;

        cpu_pause();

        /* A target that has been silent for this long is either wedged or was
         * never listening. Re-sending costs one ICR write and recovers the
         * case where the first IPI was lost to a race against the target's
         * own bring-up. */
        if ((++spins & 0xFFFFFF) == 0) {
            u64 m2 = remaining;
            while (m2) {
                u32 c = hw_ctz64(m2);
                m2 &= m2 - 1;
                lapic_send_ipi(g_cpu_infos[c].lapic_id, SMP_VEC_CALL_FUNC);
            }
        }
    }
}

int smp_call_function_mask(u64 mask, void (*func)(void *), void *info, bool wait)
{
    if (!func) return -22; /* -EINVAL */

    u32 me = smp_current_cpu_id();
    if (me >= SMP_MAX_CPUS) me = 0;

    bool call_self = (mask & (1ULL << me)) != 0;
    u64 targets = mask & smp_online_mask() & ~(1ULL << me);

    /* csds[] is indexed by target CPU so smp_call_wait() can map a bit back
     * to its request without a second array. One entry per possible CPU is
     * 2 KB of an 16 KB kernel stack — worth it to keep the whole mechanism
     * allocation-free. */
    struct smp_csd csds[SMP_MAX_CPUS];

    u64 m = targets;
    while (m) {
        u32 c = hw_ctz64(m);
        m &= m - 1;
        csds[c].next = NULL;
        csds[c].func = func;
        csds[c].info = info;
        csds[c].done = 0;
        smp_call_queue_push(c, &csds[c]);
    }

    if (targets) smp_send_ipi_mask(targets, SMP_VEC_CALL_FUNC);

    /* Run it here too, if asked. Done after the IPIs are out so the remote
     * CPUs start working in parallel with this one rather than after it. */
    if (call_self) func(info);

    /* The wait is unconditional even when the caller passed wait == false.
     * csds[] lives in this stack frame, and the targets hold pointers into it
     * until they have run the callback; returning early would hand them a
     * frame that has since been reused. There is nowhere else to put the
     * requests that does not require allocation, and this primitive has to
     * work in contexts where allocation is impossible. A caller that really
     * wants fire-and-forget wants a bare IPI rather than a function call, and
     * should use smp_send_ipi_mask() directly. */
    (void)wait;
    if (targets) smp_call_wait(csds, targets);

    return 0;
}

int smp_call_function_single(u32 cpu, void (*func)(void *), void *info, bool wait)
{
    if (cpu >= SMP_MAX_CPUS) return -22;            /* -EINVAL */
    if (cpu != smp_current_cpu_id() && !smp_cpu_online(cpu)) return -6; /* -ENXIO */
    return smp_call_function_mask(1ULL << cpu, func, info, wait);
}

int smp_call_function(void (*func)(void *), void *info, bool wait)
{
    u32 me = smp_current_cpu_id();
    if (me >= SMP_MAX_CPUS) me = 0;
    return smp_call_function_mask(smp_online_mask() & ~(1ULL << me), func, info, wait);
}

int smp_call_function_all(void (*func)(void *), void *info, bool wait)
{
    u32 me = smp_current_cpu_id();
    if (me >= SMP_MAX_CPUS) me = 0;
    return smp_call_function_mask(smp_online_mask() | (1ULL << me), func, info, wait);
}

/* ── Emergency stop ───────────────────────────────────────────────────────── */

static volatile u32 g_stop_requested = 0;
static volatile u32 g_stopped_count  = 0;

bool smp_nmi_stop_self(void)
{
    if (!__atomic_load_n(&g_stop_requested, __ATOMIC_ACQUIRE)) return false;

    __atomic_add_fetch(&g_stopped_count, 1, __ATOMIC_SEQ_CST);

    /* Leave the online mask, so nothing tries to IPI this core again, and
     * park. Deliberately no EOI: an NMI does not use the APIC's in-service
     * mechanism, and there is nothing left to return to anyway. */
    u32 me = smp_current_cpu_id();
    if (me < SMP_MAX_CPUS)
        __atomic_and_fetch(&g_cpu_online_mask, ~(1ULL << me), __ATOMIC_SEQ_CST);

    for (;;) { cpu_cli(); cpu_hlt(); }
}

void smp_stop_other_cpus(void)
{
    u32 me = smp_current_cpu_id();
    if (me >= SMP_MAX_CPUS) me = 0;

    u64 others = smp_online_mask() & ~(1ULL << me);
    if (!others) return;

    u32 expected = (u32)hw_popcnt64(others);

    __atomic_store_n(&g_stop_requested, 1, __ATOMIC_SEQ_CST);

    /* Maskable IPI first: a healthy CPU parks itself from the handler without
     * the collateral an NMI brings (an NMI can land in the middle of a
     * non-reentrant path and, worse, blocks all further NMIs on that core). */
    smp_send_ipi_mask(others, SMP_VEC_STOP);

    u64 timeout = 50000000ULL;
    while (__atomic_load_n(&g_stopped_count, __ATOMIC_SEQ_CST) < expected && timeout--)
        cpu_pause();

    if (__atomic_load_n(&g_stopped_count, __ATOMIC_SEQ_CST) >= expected) return;

    /* Whatever is left is not servicing interrupts — spinning on a lock this
     * CPU holds, or already inside a fault. NMI reaches it anyway. */
    lapic_send_nmi_allbutself();

    timeout = 50000000ULL;
    while (__atomic_load_n(&g_stopped_count, __ATOMIC_SEQ_CST) < expected && timeout--)
        cpu_pause();
}

/* Handler body for SMP_VEC_STOP. Declared here rather than in idt.c so the
 * parking policy lives next to smp_stop_other_cpus(). */
void smp_stop_interrupt(void);
void smp_stop_interrupt(void)
{
    lapic_eoi();
    __atomic_add_fetch(&g_stopped_count, 1, __ATOMIC_SEQ_CST);

    u32 me = smp_current_cpu_id();
    if (me < SMP_MAX_CPUS)
        __atomic_and_fetch(&g_cpu_online_mask, ~(1ULL << me), __ATOMIC_SEQ_CST);

    for (;;) { cpu_cli(); cpu_hlt(); }
}

/* ── AP bring-up ──────────────────────────────────────────────────────────── */

/* used: the only call site is the literal "call ap_c_entry" text inside
 * ap_entry()'s inline asm below. That's invisible to LTO's whole-program
 * reachability analysis (it only sees C-level call graphs, not assembler
 * text), so under -flto this function looks unreferenced and gets dropped
 * — silently, at link time, as an undefined-reference error pointing at
 * the asm(), not at anything wrong here. `used` tells the compiler to keep
 * it regardless of what its own analysis can see. */
__attribute__((used))
void ap_c_entry(struct limine_smp_info *info)
{
    if (!info) return;

    u32 cpu_id = (u32)info->extra_argument;
    cpu_info_t *cpu = &g_cpu_infos[cpu_id];

    /* GS_BASE = kernel cpu_info (active right now in ring 0). Set FIRST so smp_get_cpu() works. */
    wrmsr(MSR_GS_BASE, (u64)(uintptr_t)cpu);
    wrmsr(MSR_KERNEL_GS_BASE, 0ULL);

    /* Load kernel page table on AP */
    vmm_switch(vmm_kernel_space());

    /* Initialize GDT and TSS for this AP */
    gdt_init_ap(cpu_id, cpu->kernel_rsp0);

    /* Load IDT */
    idt_init();

    /* Initialize Local APIC on this CPU. Follows the BSP into x2APIC when
     * that is the mode in use — see lapic_init(). */
    lapic_init();

    /* The APIC's own ID register is the authority once the mode is settled;
     * what Limine reported was an xAPIC ID read before the transition. They
     * agree on every machine with fewer than 255 CPUs, but recording the real
     * one keeps IPI destinations correct on the machines where they do not. */
    cpu->lapic_id = lapic_id();

    /* Decode this CPU's place in the package/core/thread hierarchy. CPUID's
     * topology leaves only ever describe the CPU executing them, so this
     * genuinely has to happen here rather than on the BSP's behalf. */
    topology_detect_self(cpu_id);

    /* Enable FPU, SSE, PGE, UMIP, FSGSBASE, OSXSAVE/AVX, SMEP, SMAP, NXE on AP */
    cpu_enable_features_ap();

    /* Enable SYSCALL / SYSRET on AP */
    extern void syscall_abi_init(void);
    syscall_abi_init();

    /* Publish this CPU as a legal IPI destination. Everything a sender needs
     * — LAPIC enabled, IDT loaded, GS base valid — is true by this point, and
     * nothing before it was, which is exactly why the mask exists. */
    __atomic_or_fetch(&g_cpu_online_mask, 1ULL << cpu_id, __ATOMIC_RELEASE);
    cpu->online = true;

    /* Signal that this AP is online */
    __atomic_add_fetch(&g_aps_online, 1, __ATOMIC_SEQ_CST);

    /*
     * Wait until the BSP finishes early kernel initialization and activates
     * the scheduler — with interrupts *enabled*.
     *
     * Limine hands an AP over with IF clear, and this loop used to spin that
     * way for the whole of kernel_main(): every driver probe, every mount,
     * the entire device tree. A CPU with interrupts disabled cannot service
     * the TLB shootdown IPI (vector 251), so every cross-CPU shootdown the
     * BSP issued in that window — and driver initialisation issues plenty,
     * since replacing a live mapping owes one — found three cores that would
     * not answer, and blocked until tlb.c's resend timeout fired, tens of
     * milliseconds each, logging "[TLB] shootdown to CPUn stuck" on the way.
     * The wait was always going to end, so it was never a hang; it was just
     * boot time spent waiting for cores that were deliberately deaf.
     *
     * Enabling interrupts here is safe precisely because of what can arrive:
     * hal/irq.c routes every device IRQ to a CPU chosen at routing time, and
     * this AP's own LAPIC timer is not started until two lines below. The
     * only thing that can land on a parked AP is an IPI — which is exactly
     * what it should be answering.
     *
     * Interrupts go back off before falling through, so the timer start and
     * sched_start() below see the same state they always did.
     */
    __asm__ volatile("sti");
    while (!__atomic_load_n(&g_smp_sched_active, __ATOMIC_SEQ_CST)) {
        cpu_pause();
    }
    __asm__ volatile("cli");

    /* Start Local APIC periodic preemption timer on this AP (100 Hz = 10ms tick) */
    lapic_timer_start(100);

    /* Start CFS scheduler on this AP */
    extern void sched_start(void);
    sched_start();
}

static void ap_entry(struct limine_smp_info *info)
{
    u32 cpu_id = (u32)info->extra_argument;
    u64 new_rsp = g_cpu_infos[cpu_id].kernel_rsp0 - 16; /* BUG-F fix: must be 16-byte aligned before call */

    /* BUG-16 fix: pin new_rsp to RCX (caller-saved, not an argument register)
     * so the compiler cannot alias it with RDI.  The original `"r"(new_rsp)`
     * constraint allowed the compiler to choose RDI for new_rsp; if it did,
     * the `mov %%rcx, %%rsp` would have clobbered info (the first argument to
     * ap_c_entry) before the call could consume it.  With explicit RCX/RDI
     * constraints the register assignment is unambiguous and compiler-agnostic. */
    register u64 _rsp  __asm__("rcx") = new_rsp;
    register struct limine_smp_info *_info __asm__("rdi") = info;

    __asm__ volatile(
        "mov %%rcx, %%rsp \n\t"
        "call ap_c_entry  \n\t"
        :
        : "r"(_rsp), "r"(_info)
        : "memory"
    );
    __builtin_unreachable();
}

void smp_init(void)
{
    struct limine_smp_response *smp_resp = g_limine_smp_req.response;

    if (!smp_resp || smp_resp->cpu_count == 0) {
        kprintf("[SMP] Limine SMP response unavailable. Running in single-CPU (BSP only) mode.\n");
        g_cpu_count = 1;
    } else {
        if (smp_resp->cpu_count > SMP_MAX_CPUS) {
            /* g_cpu_infos[] and every other CPU-indexed table this kernel
             * keeps (see gdt.c's HAL_MAX_CPUS-sized table) are fixed-size at
             * compile time, so a machine with more logical processors than
             * that really did just lose the extras -- say so instead of
             * quietly scheduling on a subset of what Limine actually found. */
            kprintf("[SMP] WARNING: Limine reports %llu CPUs, only using %u (SMP_MAX_CPUS)\n",
                    (unsigned long long)smp_resp->cpu_count, (unsigned int)SMP_MAX_CPUS);
        }
        g_cpu_count = (smp_resp->cpu_count > SMP_MAX_CPUS) ? SMP_MAX_CPUS : (u32)smp_resp->cpu_count;
        kprintf("[SMP] Limine reported %u total CPU(s). Initializing APs...\n", g_cpu_count);
    }

    /* Setup BSP cpu_info (CPU 0) */
    cpu_info_t *bsp = &g_cpu_infos[0];
    __builtin_memset(bsp, 0, sizeof(cpu_info_t));
    bsp->cpu_id = 0;
    bsp->lapic_id = (smp_resp && smp_resp->cpu_count > 0) ? smp_resp->bsp_lapic_id : 0;
    bsp->is_bsp = true;
    bsp->self = bsp;

    /* Allocate and assign kernel stack for BSP */
    phys_addr_t bsp_stack_phys = pmm_alloc_pages(KERNEL_STACK_PAGES);
    if (!bsp_stack_phys) PANIC("Failed to allocate BSP kernel stack!");
    bsp->kernel_rsp0 = (u64)PHYS_TO_VIRT(bsp_stack_phys) + KERNEL_STACK_SIZE;

    /* GS_BASE  = kernel cpu_info for BSP. Set FIRST before gdt_init_ap. */
    wrmsr(MSR_GS_BASE, (u64)(uintptr_t)bsp);
    wrmsr(MSR_KERNEL_GS_BASE, 0ULL);

    /* Reinitialize GDT/TSS for BSP with the new allocated stack */
    gdt_init_ap(0, bsp->kernel_rsp0);

    /* Initialize LAPIC on BSP. This is where the machine-wide xAPIC/x2APIC
     * decision is made; every AP follows it. */
    lapic_init();
    bsp->lapic_id = lapic_id();
    bsp->online = true;
    __atomic_or_fetch(&g_cpu_online_mask, 1ULL, __ATOMIC_RELEASE);

    topology_detect_self(0);

    /* Calibrate the LAPIC timer against HPET once, on the BSP, before any
     * lapic_timer_start() call (the first is a few lines below, for the
     * single-CPU case; kernel_main()'s and every AP's follow later). The
     * bus/APIC clock this timer runs off is uniform across cores in the
     * same package/system, so one calibration covers every CPU that will
     * later call lapic_timer_start(100) expecting an actual 100 Hz. */
    lapic_timer_calibrate();

    if (!smp_resp || g_cpu_count <= 1) {
        kprintf("[SMP] BSP online (ID=0, APIC_ID=%u)\n", bsp->lapic_id);
        topology_finalize(1);
        topology_dump(1);
        return;
    }

    /* Boot all APs */
    u32 ap_id = 1; /* BSP always gets ID 0; APs get 1, 2, 3... */
    /* BUG-E fix: iterate up to g_cpu_count (already capped) rather than the raw
     * Limine count, which may exceed SMP_MAX_CPUS and waste iterations. */
    for (u32 i = 0; i < smp_resp->cpu_count && ap_id < g_cpu_count; i++) {
        struct limine_smp_info *info = smp_resp->cpus[i];
        if (info->lapic_id == smp_resp->bsp_lapic_id) continue; /* Skip BSP */

        u32 id = ap_id++;
        cpu_info_t *cpu = &g_cpu_infos[id];
        __builtin_memset(cpu, 0, sizeof(cpu_info_t));
        cpu->cpu_id = id;
        cpu->lapic_id = info->lapic_id;
        cpu->is_bsp = false;
        cpu->self = cpu;

        phys_addr_t stack_phys = pmm_alloc_pages(KERNEL_STACK_PAGES);
        if (!stack_phys) PANIC("Failed to allocate AP kernel stack!");
        cpu->kernel_rsp0 = (u64)PHYS_TO_VIRT(stack_phys) + KERNEL_STACK_SIZE;

        info->extra_argument = (u64)id;
        __atomic_store_n(&info->goto_address, ap_entry, __ATOMIC_RELEASE);
    }

    /* Wait for all APs to come online (with timeout) */
    u32 expected_aps = g_cpu_count - 1;
    u64 timeout = 2000000ULL;
    while (__atomic_load_n(&g_aps_online, __ATOMIC_SEQ_CST) < expected_aps && timeout > 0) {
        cpu_pause();
        timeout--;
    }

    /* BUG-A fix: capture the atomic load once so the comparison and the log
     * message use the same consistent value (no torn read in kprintf). */
    u32 online = __atomic_load_n(&g_aps_online, __ATOMIC_SEQ_CST);
    if (online < expected_aps) {
        kprintf("[SMP] WARNING: Only %u/%u APs came online!\n",
                online, expected_aps);
        /* A CPU that never reported in is not a legal IPI destination and
         * must not be scheduled onto either. Trimming g_cpu_count would
         * renumber the ones that did come up, so the online mask (which is
         * what every send and every placement decision consults) carries the
         * exclusion instead. */
    } else {
        kprintf("[SMP] All %u AP(s) successfully booted and online.\n", online);
    }

    topology_finalize(g_cpu_count);
    topology_dump(g_cpu_count);
}

bool smp_ap_stack_phys(u32 cpu_id, phys_addr_t *out_base, size_t *out_len)
{
    if (cpu_id == 0 || cpu_id >= g_cpu_count) return false;
    cpu_info_t *cpu = &g_cpu_infos[cpu_id];
    if (!cpu->kernel_rsp0) return false;
    if (out_base) *out_base = VIRT_TO_PHYS(cpu->kernel_rsp0 - KERNEL_STACK_SIZE);
    if (out_len)  *out_len  = KERNEL_STACK_SIZE;
    return true;
}

void smp_send_ipi(u32 cpu_id, u8 vector)
{
    if (cpu_id >= g_cpu_count || cpu_id == smp_current_cpu_id()) return;
    if (!smp_cpu_online(cpu_id)) return;

    cpu_info_t *self = smp_get_cpu();
    if (self) self->ipis_sent++;
    lapic_send_ipi(g_cpu_infos[cpu_id].lapic_id, vector);
}

void smp_send_ipi_mask(u64 mask, u8 vector)
{
    u32 me = smp_current_cpu_id();
    if (me >= SMP_MAX_CPUS) me = 0;

    u64 online = smp_online_mask();
    u64 targets = mask & online & ~(1ULL << me);
    if (!targets) return;

    /* When the target set is literally "every other online CPU", the ICR's
     * all-excluding-self shorthand does it in one write instead of one per
     * CPU. On a 64-thread machine that is the difference between one ICR
     * write and 63 — and the shootdown path takes it on every unmap. */
    if (targets == (online & ~(1ULL << me))) {
        cpu_info_t *self = smp_get_cpu();
        if (self) self->ipis_sent += (u64)hw_popcnt64(targets);
        lapic_send_ipi_allbutself(vector);
        return;
    }

    while (targets) {
        u32 c = hw_ctz64(targets);
        targets &= targets - 1;
        cpu_info_t *self = smp_get_cpu();
        if (self) self->ipis_sent++;
        lapic_send_ipi(g_cpu_infos[c].lapic_id, vector);
    }
}

void smp_send_ipi_allbutself(u8 vector)
{
    u32 me = smp_current_cpu_id();
    if (me >= SMP_MAX_CPUS) me = 0;
    smp_send_ipi_mask(smp_online_mask() & ~(1ULL << me), vector);
}

void smp_send_reschedule(u32 cpu_id)
{
    if (cpu_id >= g_cpu_count || cpu_id == smp_current_cpu_id()) return;
    if (!smp_cpu_online(cpu_id)) return;

    cpu_info_t *self = smp_get_cpu();
    if (self) { self->ipis_sent++; self->ipis_resched++; }
    lapic_send_ipi(g_cpu_infos[cpu_id].lapic_id, SMP_VEC_RESCHEDULE);
}
