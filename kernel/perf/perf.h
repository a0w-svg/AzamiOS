/* Counting-only Linux perf ABI: task events accept pid 0 or a live TID and
 * CPU -1 or a CPU filter; pid -1 requires an online CPU for system-wide events.
 * Software clocks count monotonic context time. Task enabled time excludes
 * descheduling; running time additionally excludes CPUs outside the filter.
 * CPU-wide clocks include idle time. RESET preserves both time accumulators.
 * Occurrences use scheduler/fault hooks and enabled/privilege/CPU filters.
 * Hardware deltas use width-masked reads on the CPU owning each interval.
 * Remote read/control uses bounded by-value requests; timeout is ETIMEDOUT.
 * Events keep IDs, never borrowed task pointers; retirement freezes counts.
 * Inheritance, enable-on-exec, groups, multiplexing and sampling are unsupported.
 */
#pragma once

#include "../sched/sched.h"
#include "../../arch/x86_64/cpu/idt.h"

/* ── attr.type ───────────────────────────────────────────────────────────── */
#define PERF_TYPE_HARDWARE    0
#define PERF_TYPE_SOFTWARE    1
#define PERF_TYPE_TRACEPOINT  2
#define PERF_TYPE_HW_CACHE    3
#define PERF_TYPE_RAW         4
#define PERF_TYPE_BREAKPOINT  5

/* ── attr.config for PERF_TYPE_HARDWARE ──────────────────────────────────── */
#define PERF_COUNT_HW_CPU_CYCLES              0
#define PERF_COUNT_HW_INSTRUCTIONS            1
#define PERF_COUNT_HW_CACHE_REFERENCES        2
#define PERF_COUNT_HW_CACHE_MISSES            3
#define PERF_COUNT_HW_BRANCH_INSTRUCTIONS     4
#define PERF_COUNT_HW_BRANCH_MISSES           5
#define PERF_COUNT_HW_BUS_CYCLES              6
#define PERF_COUNT_HW_STALLED_CYCLES_FRONTEND 7
#define PERF_COUNT_HW_STALLED_CYCLES_BACKEND  8
#define PERF_COUNT_HW_REF_CPU_CYCLES          9
#define PERF_COUNT_HW_MAX                     10

/* ── attr.config for PERF_TYPE_SOFTWARE ──────────────────────────────────── */
#define PERF_COUNT_SW_CPU_CLOCK         0
#define PERF_COUNT_SW_TASK_CLOCK        1
#define PERF_COUNT_SW_PAGE_FAULTS       2
#define PERF_COUNT_SW_CONTEXT_SWITCHES  3
#define PERF_COUNT_SW_CPU_MIGRATIONS    4
#define PERF_COUNT_SW_PAGE_FAULTS_MIN   5
#define PERF_COUNT_SW_PAGE_FAULTS_MAJ   6
#define PERF_COUNT_SW_ALIGNMENT_FAULTS  7
#define PERF_COUNT_SW_EMULATION_FAULTS  8
#define PERF_COUNT_SW_DUMMY             9
#define PERF_COUNT_SW_MAX               10

/* ── attr.read_format ────────────────────────────────────────────────────── */
#define PERF_FORMAT_TOTAL_TIME_ENABLED  (1U << 0)
#define PERF_FORMAT_TOTAL_TIME_RUNNING  (1U << 1)
#define PERF_FORMAT_ID                  (1U << 2)
#define PERF_FORMAT_GROUP               (1U << 3)

/* ── attr flag bits (the anonymous bitfield in Linux's struct) ───────────── */
#define PERF_ATTR_DISABLED       (1ULL << 0)
#define PERF_ATTR_INHERIT        (1ULL << 1)
#define PERF_ATTR_PINNED         (1ULL << 2)
#define PERF_ATTR_EXCLUSIVE      (1ULL << 3)
#define PERF_ATTR_EXCLUDE_USER   (1ULL << 4)
#define PERF_ATTR_EXCLUDE_KERNEL (1ULL << 5)
#define PERF_ATTR_EXCLUDE_HV     (1ULL << 6)
#define PERF_ATTR_EXCLUDE_IDLE   (1ULL << 7)
#define PERF_ATTR_ENABLE_ON_EXEC (1ULL << 12)
#define PERF_ATTR_EXCLUDE_GUEST  (1ULL << 20)

/* Counting accepts this record-layout metadata without producing samples. */
#define PERF_SAMPLE_IDENTIFIER  (1ULL << 16)

/* ── perf_event_open(2) flags ────────────────────────────────────────────── */
#define PERF_FLAG_FD_NO_GROUP  (1UL << 0)
#define PERF_FLAG_FD_OUTPUT    (1UL << 1)
#define PERF_FLAG_PID_CGROUP   (1UL << 2)
#define PERF_FLAG_FD_CLOEXEC   (1UL << 3)

/* ── ioctl(2) requests. Linux encodes these as _IO('$', n). ──────────────── */
#define PERF_IOC_MAGIC     '$'
#define PERF_IOC_ENABLE    0
#define PERF_IOC_DISABLE   1
#define PERF_IOC_REFRESH   2
#define PERF_IOC_RESET     3
#define PERF_IOC_PERIOD    4
#define PERF_IOC_SET_OUTPUT 5
#define PERF_IOC_SET_FILTER 6
#define PERF_IOC_ID        7

/**
 * struct perf_event_attr — the Linux attribute block, prefix only.
 *
 * Userspace declares how much of it it knows about in `size`, and every
 * version is a strict prefix of the next, so a short attr is zero-extended and
 * a longer one from a newer libc is accepted as long as the bytes past what we
 * understand are zero. Only the fields this kernel acts on are named.
 */
struct perf_event_attr {
    u32 type;
    u32 size;
    u64 config;
    u64 sample_period;       /* union with sample_freq */
    u64 sample_type;
    u64 read_format;
    u64 flags;               /* PERF_ATTR_* — Linux's bitfield word */
    u32 wakeup_events;
    u32 bp_type;
    u64 config1;
    u64 config2;
};

#define PERF_ATTR_SIZE_VER0  64
#define PERF_ATTR_SIZE_MAX   136

/** perf_init() — set up the event table. Call after pmu_init(). */
void perf_init(void);

/** sys_perf_event_open_impl() — registered as SYS_perf_event_open. */
s64 sys_perf_event_open_impl(pt_regs_t *r);

/**
 * perf_sched_switch(prev, next) — the context-switch hook.
 *
 * Maintains process statistics and thread event intervals, and closes
 * out @prev's hardware deltas before opening @next's. Runs on every context
 * switch; the no-events path also synchronizes pending PMU slot changes.
 *
 * Called from sched_post_switch(), i.e. already in @next's context on the core
 * both threads ran on — which is exactly what makes the hardware delta
 * attributable to @prev and nobody else.
 */
void perf_sched_switch(thread_t *prev, thread_t *next);
void perf_thread_exit(thread_t *t);
void perf_page_fault(bool user);
extern s32 g_perf_event_paranoid;

/**
 * perf_process_exit(p) — freeze every remaining event owned by @p.
 *
 * The event stays open — its fd may be held by a parent that has not read it
 * yet — and its final count remains readable; it simply stops matching the
 * pid, which it must do before that number is handed to a new process.
 */
void perf_process_exit(process_t *p);
