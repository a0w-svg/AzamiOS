/* ============================================================================
 * AzamiOS — Performance Monitoring Unit (x86_64)
 * File: arch/x86_64/cpu/pmu.h
 *
 * The hardware half of perf_event_open(2): what the CPU can count, and the
 * MSR programming that makes it count. kernel/perf/perf.c owns the file
 * descriptors, the per-task arithmetic and the policy; this file owns nothing
 * but the counters.
 *
 * How a counter gets shared
 * -------------------------
 * The PMU MSRs are per *logical processor*, so "program a counter" really
 * means "program the same counter on every core". Rather than broadcast an IPI
 * for a facility that is only used while something is being measured, a slot
 * is claimed in a small global table and a generation number is bumped;
 * pmu_sync_local() replays the table onto whichever core notices the change
 * first, and it is called from the context-switch path, so a core reprograms
 * itself right before it next runs a task. A core that is idle and never
 * switches has nothing to count anyway.
 *
 * Counters therefore free-run: they are never started, stopped or reset per
 * task. Per-task attribution is a subtraction — read at switch-in, read again
 * at switch-out, add the difference — which is exact because a thread cannot
 * migrate between those two points, so both reads come from the same core's
 * counter. Wrap is handled by masking to the counter's real width.
 *
 * What is deliberately absent
 * ---------------------------
 * No overflow interrupt and no PEBS, so there is no sampling — this counts,
 * it does not profile. Fixed-function counters are not used either: they would
 * be a second read path (RDPMC index 0x4000000n) and a second programming path
 * for events the general-purpose counters already cover, on a machine that has
 * never run out of general-purpose slots.
 *
 * On QEMU without `-cpu host` or `+pmu` the guest enumerates no PMU at all;
 * everything here reports "not present" and perf_event_open() answers hardware
 * requests with -ENOENT while software events keep working. That is the
 * expected state on the default machine, not a failure.
 * ============================================================================ */
#pragma once

#include "../../../include/azami/types.h"

#define PMU_MAX_COUNTERS   8

/* IA32_PERFEVTSEL / AMD PerfEvtSel layout. */
#define PMU_EVTSEL_EVENT(x)  ((u64)((x) & 0xFF))
#define PMU_EVTSEL_UMASK(x)  (((u64)((x) & 0xFF)) << 8)
#define PMU_EVTSEL_USR       (1ULL << 16)   /* count at CPL > 0  */
#define PMU_EVTSEL_OS        (1ULL << 17)   /* count at CPL == 0 */
#define PMU_EVTSEL_EDGE      (1ULL << 18)
#define PMU_EVTSEL_INT       (1ULL << 20)
#define PMU_EVTSEL_EN        (1ULL << 22)
#define PMU_EVTSEL_INV       (1ULL << 23)
#define PMU_EVTSEL_CMASK(x)  (((u64)((x) & 0xFF)) << 24)
/* The bits a PERF_TYPE_RAW config is allowed to set: event, umask, edge,
 * invert and cmask. USR/OS come from the event's exclude_* attributes and INT
 * is refused outright — arming an overflow interrupt for a handler that does
 * not exist would wedge the core. */
#define PMU_EVTSEL_RAW_MASK  0x00FFFFFFULL

typedef struct {
    bool present;        /* a usable PMU was found                          */
    bool is_amd;         /* AMD counter MSRs rather than Intel's            */
    bool has_global_ctrl;/* Intel arch PMU v2+: counters need arming        */
    u32  version;        /* Intel architectural PMU version, 0 on AMD       */
    u32  nr_counters;    /* general-purpose counters per logical processor  */
    u32  width;          /* counter width in bits                          */
    u64  mask;           /* (1 << width) - 1, for wrap-safe subtraction     */
    u32  unavail_mask;   /* CPUID.0xA:EBX — architectural events NOT usable */
} pmu_info_t;

extern pmu_info_t g_pmu;

/** pmu_init() — probe the PMU on the BSP. Call after cpu_detect_features(). */
void pmu_init(void);

/** pmu_init_ap() — bring this application processor's counters into line. */
void pmu_init_ap(void);

/** pmu_format(buf, max) — one-line description for /proc/cpuinfo. */
size_t pmu_format(char *buf, size_t max);

/**
 * pmu_slot_acquire(evtsel) — claim a counter for @evtsel.
 *
 * Slots are shared: asking for an event that is already programmed hands back
 * the same slot with one more reference, which is what lets several processes
 * count instructions at once on a four-counter part. Returns the slot index,
 * or -1 when there is no PMU or every slot holds a different event.
 */
int pmu_slot_acquire(u64 evtsel);

/** pmu_slot_release(slot) — drop one reference; frees the slot at zero. */
void pmu_slot_release(int slot);

/**
 * pmu_slot_read(slot) — this core's raw count for @slot.
 *
 * Only meaningful against another read from the same core; see the note on
 * per-task attribution above. Returns 0 for an unprogrammed slot rather than
 * executing RDPMC on an index the CPU would fault on.
 */
u64 pmu_slot_read(int slot);

/**
 * pmu_sync_local() — reprogram this core if the slot table changed.
 *
 * One relaxed load and a compare in the common case. Call it from the context
 * switch path, before a task that may be counting is resumed.
 */
void pmu_sync_local(void);

/**
 * pmu_hw_event(hw_config) — PERF_COUNT_HW_* → a PERFEVTSEL event/umask pair.
 *
 * Returns 0 when this CPU cannot count that event, either because CPUID says
 * the architectural event is unavailable or because no vendor-neutral encoding
 * for it exists on this part. Callers must treat 0 as "unsupported" and not as
 * a valid selector.
 */
u64 pmu_hw_event(u64 hw_config);
