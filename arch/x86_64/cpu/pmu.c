/* ============================================================================
 * AzamiOS — Performance Monitoring Unit (x86_64)
 * File: arch/x86_64/cpu/pmu.c
 *
 * See pmu.h for the model: free-running counters, a globally shared slot
 * table, and per-core reprogramming driven by a generation counter.
 * ========================================================================= */

#define DEBUG 0
#include <azami/debug.h>

#include "pmu.h"
#include "cpu.h"
#include "msr.h"
#include "smp.h"
#include "spinlock.h"
#include "../../../drivers/char/console.h"
#include "../../../kernel/lib/string.h"
#include "../../../include/azami/defs.h"

pmu_info_t g_pmu;

/* The slot table. Guarded by g_pmu_lock for mutation; readers on the context
 * switch path take a snapshot under the same lock, which is why the table is
 * eight entries of two words rather than anything that needs traversal. */
static spinlock_t g_pmu_lock = SPINLOCK_INIT;
static u64 g_slot_evtsel[PMU_MAX_COUNTERS];
static u32 g_slot_refs[PMU_MAX_COUNTERS];

/* Bumped on every change to the table above. A core reprograms itself when its
 * own copy falls behind; nothing else has to be broadcast. */
static volatile u64 g_pmu_gen;
static u64 g_pmu_local_gen[SMP_MAX_CPUS];

/* ── MSR addressing ──────────────────────────────────────────────────────── */

/* AMD parts that enumerate PerfCtrExtCore put six counters at a strided pair
 * of MSRs; everything else uses the four legacy K7 ones. */
static bool g_amd_ext;

static inline u32 evtsel_msr(u32 i)
{
    if (!g_pmu.is_amd) return (u32)(MSR_IA32_PERFEVTSEL0 + i);
    return g_amd_ext ? (u32)(MSR_AMD_PERFEVTSEL_EXT0 + 2 * i)
                     : (u32)(MSR_K7_PERFEVTSEL0 + i);
}

static inline u32 counter_msr(u32 i)
{
    if (!g_pmu.is_amd) return (u32)(MSR_IA32_PMC0 + i);
    return g_amd_ext ? (u32)(MSR_AMD_PERFCTR_EXT0 + 2 * i)
                     : (u32)(MSR_K7_PERFCTR0 + i);
}

static inline u64 rdpmc_raw(u32 idx)
{
    u32 lo, hi;
    __asm__ volatile("rdpmc" : "=a"(lo), "=d"(hi) : "c"(idx));
    return ((u64)hi << 32) | lo;
}

/* hwprobe.asm — runs one candidate instruction behind an .extable fixup and
 * reports whether it faulted. Shared with the hwaccel dispatcher, which needs
 * exactly the same "CPUID said yes, but does it actually execute?" test. */
extern int hwaccel_probe_asm(void *scratch, u32 which);
#define PROBE_RDPMC  4

/* ── Detection ───────────────────────────────────────────────────────────── */

void pmu_init(void)
{
    u32 eax, ebx, ecx, edx;

    __builtin_memset(&g_pmu, 0, sizeof(g_pmu));
    for (u32 i = 0; i < SMP_MAX_CPUS; i++) g_pmu_local_gen[i] = 0;

    if (cpu_is_intel() && g_cpu_info.max_leaf >= 0x0A) {
        cpuid(0x0A, 0, &eax, &ebx, &ecx, &edx);
        u32 version = eax & 0xFF;
        u32 nr      = (eax >> 8)  & 0xFF;
        u32 width   = (eax >> 16) & 0xFF;
        /* Version 0 means "no architectural PMU", and a part that claims one
         * but no counters or a nonsensical width is not usable either. */
        if (version > 0 && nr > 0 && width >= 32 && width <= 64) {
            g_pmu.present         = true;
            g_pmu.version         = version;
            g_pmu.nr_counters     = nr > PMU_MAX_COUNTERS ? PMU_MAX_COUNTERS : nr;
            g_pmu.width           = width;
            g_pmu.has_global_ctrl = (version >= 2);
            /* EBX's bit vector marks architectural events that are *not*
             * available; only the bits CPUID says it enumerated are valid. */
            u32 ebx_len = (eax >> 24) & 0xFF;
            g_pmu.unavail_mask = (ebx_len >= 32) ? ebx
                               : (ebx_len ? (ebx & ((1u << ebx_len) - 1)) : 0);
        }
    } else if (cpu_is_amd()) {
        /* Every 64-bit AMD part has the four K7-style counters; the extended
         * six are enumerated in CPUID 0x80000001 ECX bit 23. */
        g_amd_ext = false;
        if (g_cpu_info.max_ext_leaf >= 0x80000001) {
            cpuid(0x80000001, 0, &eax, &ebx, &ecx, &edx);
            g_amd_ext = (ecx & (1u << 23)) != 0;
        }
        g_pmu.present     = true;
        g_pmu.is_amd      = true;
        g_pmu.nr_counters = g_amd_ext ? 6 : 4;
        g_pmu.width       = 48;
    }

    if (!g_pmu.present) {
        kprintf("[PMU] no performance counters (perf: software events only)\n");
        return;
    }

    g_pmu.mask = (g_pmu.width >= 64) ? ~0ULL : ((1ULL << g_pmu.width) - 1);

    /* Start from a known state: nothing selected, nothing armed. */
    for (u32 i = 0; i < g_pmu.nr_counters; i++) {
        wrmsr(evtsel_msr(i), 0);
        wrmsr(counter_msr(i), 0);
    }
    if (g_pmu.has_global_ctrl) {
        wrmsr(MSR_IA32_PERF_GLOBAL_CTRL, 0);
        wrmsr(MSR_IA32_PERF_GLOBAL_OVF_CTRL, ~0ULL);
    }

    /* Everything above trusted CPUID, and CPUID is a claim rather than a
     * guarantee. QEMU's TCG is the case that matters here: it happily reports
     * a PMU and accepts every PERFEVTSEL write, then raises #UD the first time
     * RDPMC executes — which, from a context-switch hook in ring 0, is an
     * unrecoverable panic rather than a missing feature. Reading a counter is
     * the only test that distinguishes the two, so do it once, here, where a
     * fault is survivable. */
    {
        u8 scratch[128] __attribute__((aligned(64)));
        if (hwaccel_probe_asm(scratch, PROBE_RDPMC) != 0) {
            for (u32 i = 0; i < g_pmu.nr_counters; i++) wrmsr(evtsel_msr(i), 0);
            if (g_pmu.has_global_ctrl) wrmsr(MSR_IA32_PERF_GLOBAL_CTRL, 0);
            __builtin_memset(&g_pmu, 0, sizeof(g_pmu));
            kprintf("[PMU] CPUID enumerates counters but RDPMC faults — "
                    "PMU disabled (perf: software events only)\n");
            return;
        }
    }

    kprintf("[PMU] %s PMU%s: %u counters, %u-bit\n",
            g_pmu.is_amd ? "AMD" : "Intel architectural",
            g_pmu.is_amd ? "" : (g_pmu.version == 1 ? " v1" :
                                 g_pmu.version == 2 ? " v2" :
                                 g_pmu.version == 3 ? " v3" : " v4+"),
            g_pmu.nr_counters, g_pmu.width);
}

void pmu_init_ap(void)
{
    if (!g_pmu.present) return;
    for (u32 i = 0; i < g_pmu.nr_counters; i++) {
        wrmsr(evtsel_msr(i), 0);
        wrmsr(counter_msr(i), 0);
    }
    if (g_pmu.has_global_ctrl) {
        wrmsr(MSR_IA32_PERF_GLOBAL_CTRL, 0);
        wrmsr(MSR_IA32_PERF_GLOBAL_OVF_CTRL, ~0ULL);
    }
    u32 cpu = smp_current_cpu_id();
    if (cpu < SMP_MAX_CPUS) g_pmu_local_gen[cpu] = 0;
    pmu_sync_local();
}

size_t pmu_format(char *buf, size_t max)
{
    if (!buf || max == 0) return 0;
    if (!g_pmu.present) return (size_t)scnprintf(buf, max, "none");
    if (g_pmu.is_amd)
        return (size_t)scnprintf(buf, max, "amd %u x %u-bit",
                                 g_pmu.nr_counters, g_pmu.width);
    return (size_t)scnprintf(buf, max, "intel-arch v%u %u x %u-bit",
                             g_pmu.version, g_pmu.nr_counters, g_pmu.width);
}

/* ── Slot table ──────────────────────────────────────────────────────────── */

int pmu_slot_acquire(u64 evtsel)
{
    if (!g_pmu.present || !evtsel) return -1;

    irqflags_t fl = spinlock_lock_irqsave(&g_pmu_lock);

    /* Share an identical selector rather than burning a second counter on it:
     * two processes counting instructions is the common case, and a four-slot
     * part runs out fast otherwise. */
    for (u32 i = 0; i < g_pmu.nr_counters; i++) {
        if (g_slot_refs[i] && g_slot_evtsel[i] == evtsel) {
            g_slot_refs[i]++;
            spinlock_unlock_irqrestore(&g_pmu_lock, fl);
            return (int)i;
        }
    }
    for (u32 i = 0; i < g_pmu.nr_counters; i++) {
        if (!g_slot_refs[i]) {
            g_slot_evtsel[i] = evtsel;
            g_slot_refs[i]   = 1;
            __atomic_add_fetch(&g_pmu_gen, 1, __ATOMIC_RELEASE);
            spinlock_unlock_irqrestore(&g_pmu_lock, fl);
            pmu_sync_local();   /* arm it here so an immediate read is valid */
            return (int)i;
        }
    }
    spinlock_unlock_irqrestore(&g_pmu_lock, fl);
    return -1;
}

void pmu_slot_release(int slot)
{
    if (!g_pmu.present || slot < 0 || (u32)slot >= g_pmu.nr_counters) return;

    irqflags_t fl = spinlock_lock_irqsave(&g_pmu_lock);
    if (g_slot_refs[slot] && --g_slot_refs[slot] == 0) {
        g_slot_evtsel[slot] = 0;
        __atomic_add_fetch(&g_pmu_gen, 1, __ATOMIC_RELEASE);
    }
    spinlock_unlock_irqrestore(&g_pmu_lock, fl);
}

u64 pmu_slot_read(int slot)
{
    if (!g_pmu.present || slot < 0 || (u32)slot >= g_pmu.nr_counters) return 0;
    /* RDPMC on an index the CPU does not implement raises #GP in ring 0, so
     * never issue it for a slot that holds no selector. */
    if (!__atomic_load_n(&g_slot_refs[slot], __ATOMIC_RELAXED)) return 0;
    return rdpmc_raw((u32)slot) & g_pmu.mask;
}

void pmu_sync_local(void)
{
    if (!g_pmu.present) return;

    u32 cpu = smp_current_cpu_id();
    if (cpu >= SMP_MAX_CPUS) return;

    u64 gen = __atomic_load_n(&g_pmu_gen, __ATOMIC_ACQUIRE);
    if (g_pmu_local_gen[cpu] == gen) return;      /* the whole hot path */

    irqflags_t fl = spinlock_lock_irqsave(&g_pmu_lock);
    u64 armed = 0;
    for (u32 i = 0; i < g_pmu.nr_counters; i++) {
        u64 sel = g_slot_refs[i] ? g_slot_evtsel[i] : 0;
        /* Disarm before rewriting the selector: changing what a counter counts
         * while it is enabled leaves a few events of the old kind in it. */
        wrmsr(evtsel_msr(i), 0);
        if (sel) {
            wrmsr(counter_msr(i), 0);
            wrmsr(evtsel_msr(i), sel);
            armed |= (1ULL << i);
        }
    }
    if (g_pmu.has_global_ctrl) wrmsr(MSR_IA32_PERF_GLOBAL_CTRL, armed);
    g_pmu_local_gen[cpu] = gen;
    spinlock_unlock_irqrestore(&g_pmu_lock, fl);
}

/* ── Event encoding ──────────────────────────────────────────────────────── */

/* PERF_COUNT_HW_* (kernel/perf/perf.h keeps the names; the numbering is the
 * Linux ABI's and is repeated here so this file stays free of that header). */
#define HW_CPU_CYCLES               0
#define HW_INSTRUCTIONS             1
#define HW_CACHE_REFERENCES         2
#define HW_CACHE_MISSES             3
#define HW_BRANCH_INSTRUCTIONS      4
#define HW_BRANCH_MISSES            5
#define HW_BUS_CYCLES               6
#define HW_STALLED_CYCLES_FRONTEND  7
#define HW_STALLED_CYCLES_BACKEND   8
#define HW_REF_CPU_CYCLES           9

/* Bit position in CPUID.0xA:EBX that marks each architectural event as
 * unavailable. -1 for events EBX says nothing about. */
static int intel_unavail_bit(u64 hw)
{
    switch (hw) {
    case HW_CPU_CYCLES:          return 0;
    case HW_INSTRUCTIONS:        return 1;
    case HW_REF_CPU_CYCLES:      return 2;
    case HW_BUS_CYCLES:          return 2;   /* same UnHalted Reference Cycles */
    case HW_CACHE_REFERENCES:    return 3;
    case HW_CACHE_MISSES:        return 4;
    case HW_BRANCH_INSTRUCTIONS: return 5;
    case HW_BRANCH_MISSES:       return 6;
    default:                     return -1;
    }
}

u64 pmu_hw_event(u64 hw_config)
{
    if (!g_pmu.present) return 0;

    u32 event = 0, umask = 0;

    if (g_pmu.is_amd) {
        /* Only the four events with a stable encoding across every AMD family
         * this kernel might run on. The cache events moved between families
         * (0x77/0x7D on K10, 0xFF60/0x0964 on Zen), and guessing wrong reports
         * a confidently wrong number, which is worse than reporting none. */
        switch (hw_config) {
        case HW_CPU_CYCLES:          event = 0x76; break;
        case HW_INSTRUCTIONS:        event = 0xC0; break;
        case HW_BRANCH_INSTRUCTIONS: event = 0xC2; break;
        case HW_BRANCH_MISSES:       event = 0xC3; break;
        default: return 0;
        }
    } else {
        /* Intel architectural events, SDM Vol 3B Table 20-1. */
        switch (hw_config) {
        case HW_CPU_CYCLES:          event = 0x3C; umask = 0x00; break;
        case HW_INSTRUCTIONS:        event = 0xC0; umask = 0x00; break;
        case HW_CACHE_REFERENCES:    event = 0x2E; umask = 0x4F; break;
        case HW_CACHE_MISSES:        event = 0x2E; umask = 0x41; break;
        case HW_BRANCH_INSTRUCTIONS: event = 0xC4; umask = 0x00; break;
        case HW_BRANCH_MISSES:       event = 0xC5; umask = 0x00; break;
        case HW_BUS_CYCLES:
        case HW_REF_CPU_CYCLES:      event = 0x3C; umask = 0x01; break;
        /* Frontend/backend stalls have no architectural encoding — they are
         * per-microarchitecture, and this kernel does not carry a model table. */
        default: return 0;
        }
        int bit = intel_unavail_bit(hw_config);
        if (bit >= 0 && (g_pmu.unavail_mask & (1u << bit))) return 0;
    }

    return PMU_EVTSEL_EVENT(event) | PMU_EVTSEL_UMASK(umask);
}
