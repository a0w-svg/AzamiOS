/* ============================================================================
 * AzamiOS — perf_event_open(2)
 * File: kernel/perf/perf.h
 *
 * Counting, not profiling. An event is a file descriptor; read(2) on it returns
 * how many times the thing happened, and ioctl(2) starts, stops and resets it.
 * That is enough for `perf stat`, for a benchmark harness that wants cycles and
 * instructions around a region, and for anything that wants to know how often
 * a process faulted or was rescheduled.
 *
 * Sampling is deliberately absent
 * -------------------------------
 * There is no ring buffer, no PERF_RECORD_* stream and no overflow interrupt,
 * so an attr with a sample_period or sample_freq is rejected outright rather
 * than accepted and quietly ignored — a profiler that thinks it is sampling and
 * receives nothing is a worse failure than one that is told no. mmap(2) on a
 * perf fd fails for the same reason.
 *
 * What a counter is attached to
 * -----------------------------
 *   pid > 0 / pid == 0   count only while that process runs. Hardware counts
 *                        are accumulated across context switches by
 *                        perf_sched_switch(); software counts are the
 *                        difference between two reads of a process_t counter.
 *   pid == -1            system-wide. Hardware and raw events sum every core's
 *                        free-running counter as each core switches; software
 *                        events other than cpu-clock have no system-wide
 *                        source here and are refused.
 *
 * The `cpu` argument is accepted and remembered but does not restrict counting:
 * a per-CPU event would need to read a counter MSR on a core this thread is not
 * running on, and there is no cross-CPU call facility to do that with.
 *
 * Accuracy, honestly
 * ------------------
 * Hardware attribution is exact to the context switch: the delta is taken
 * between a read at switch-in and a read at switch-out on the same core, so no
 * other task's work lands in it. What does land in it is the handful of kernel
 * instructions between the switch itself and the hook — sub-microsecond, and
 * constant, but not zero. exclude_user/exclude_kernel are honoured by the
 * PERFEVTSEL bits, so those are exact.
 *
 * Time is measured in scheduler ticks (10 ms) and reported in nanoseconds, so
 * time_enabled, time_running, cpu-clock and task-clock are 10 ms granular. They
 * are not derived from the TSC because nothing in this kernel calibrates it.
 * ============================================================================ */
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
 * Maintains the per-process counters every software event reads, and closes
 * out @prev's hardware deltas before opening @next's. Runs on every context
 * switch, so the no-events case is one relaxed load and a branch.
 *
 * Called from sched_post_switch(), i.e. already in @next's context on the core
 * both threads ran on — which is exactly what makes the hardware delta
 * attributable to @prev and nobody else.
 */
void perf_sched_switch(process_t *prev, process_t *next);

/**
 * perf_process_exit(p) — detach every event still pointing at @p.
 *
 * The event stays open — its fd may be held by a parent that has not read it
 * yet — and its final count remains readable; it simply stops matching the
 * pid, which it must do before that number is handed to a new process.
 */
void perf_process_exit(process_t *p);
