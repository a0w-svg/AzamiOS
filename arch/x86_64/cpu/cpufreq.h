/* ============================================================================
 * AzamiOS — CPU Frequency Scaling & Hardware P-State (cpufreq) Header
 * File: arch/x86_64/cpu/cpufreq.h
 *
 * Implements autonomous hardware-controlled frequency and performance scaling:
 *   - Intel HWP (Speed Shift): MSR_IA32_PM_ENABLE (0x770), HWP_CAPABILITIES (0x771),
 *     HWP_REQUEST (0x774)
 *   - Intel Energy Performance Bias (EPB): MSR_IA32_ENERGY_PERF_BIAS (0x1B0)
 *   - AMD CPPC (Collaborative Processor Performance Control): MSR 0xC00102B0/0xC00102B1
 *   - APERF / MPERF hardware frequency counters (MSR 0xE7 / 0xE8)
 *
 * Exposes Linux-compatible sysfs nodes under /sys/devices/system/cpu/cpuN/cpufreq/
 * ============================================================================ */
#pragma once

#include "../../../include/azami/types.h"
#include "../../../include/azami/defs.h"
#include "smp.h"

/* ── MSR Definitions ──────────────────────────────────────────────────────── */
#define MSR_IA32_APERF             0x000000E7U
#define MSR_IA32_MPERF             0x000000E8U
#define MSR_IA32_ENERGY_PERF_BIAS  0x000001B0U
#define MSR_IA32_PERF_STATUS       0x00000198U
#define MSR_IA32_PERF_CTL          0x00000199U

/* Intel HWP (Hardware P-States / Speed Shift) */
#define MSR_IA32_PM_ENABLE         0x00000770U
#define MSR_IA32_HWP_CAPABILITIES  0x00000771U
#define MSR_IA32_HWP_REQUEST_PKG   0x00000772U
#define MSR_IA32_HWP_INTERRUPT     0x00000773U
#define MSR_IA32_HWP_REQUEST       0x00000774U
#define MSR_IA32_HWP_STATUS        0x00000777U

/* AMD CPPC (Collaborative Processor Performance Control) */
#define MSR_AMD_CPPC_CAP1          0xC00102B0U
#define MSR_AMD_CPPC_ENABLE        0xC00102B1U
#define MSR_AMD_CPPC_REQ           0xC00102B2U
#define MSR_AMD_CPPC_STATUS        0xC00102B3U

/* Energy Performance Preference (EPP) Profiles */
#define HWP_EPP_PERFORMANCE        0x00   /* Maximum turbo, no delay */
#define HWP_EPP_BALANCE_PERF       0x40   /* High responsiveness, slight power saving */
#define HWP_EPP_NORMAL             0x80   /* Balanced */
#define HWP_EPP_BALANCE_POW        0xC0   /* Power saving priority */
#define HWP_EPP_POWERSAVE          0xFF   /* Maximum battery life */

typedef enum {
    CPUFREQ_GOV_PERFORMANCE,
    CPUFREQ_GOV_POWERSAVE,
    CPUFREQ_GOV_ONDEMAND,
} cpufreq_governor_t;

typedef struct cpu_perf_state {
    bool     hwp_supported;
    bool     cppc_supported;
    bool     aperf_mperf_supported;

    u32      base_freq_khz;
    u32      max_freq_khz;
    u32      min_freq_khz;
    u32      cur_freq_khz;

    /* Performance level registers (1 = lowest, 255 = highest) */
    u8       highest_perf;
    u8       guaranteed_perf;
    u8       efficient_perf;
    u8       lowest_perf;
    u8       epp;

    cpufreq_governor_t governor;

    /* Last APERF/MPERF snapshots */
    u64      last_aperf;
    u64      last_mperf;
    u64      last_sample_ns;
} cpu_perf_state_t;

/** cpufreq_init() — Discover and enable hardware frequency & P-state scaling. */
void cpufreq_init(void);

/** cpufreq_get_cur_freq(cpu) — Return current measured CPU frequency in kHz. */
u32 cpufreq_get_cur_freq(u32 cpu);

/** cpufreq_get_max_freq(cpu) — Return maximum turbo CPU frequency in kHz. */
u32 cpufreq_get_max_freq(u32 cpu);

/** cpufreq_get_min_freq(cpu) — Return lowest throttle CPU frequency in kHz. */
u32 cpufreq_get_min_freq(u32 cpu);

/** cpufreq_set_governor(cpu, gov) — Change CPU governor (performance, powersave, ondemand). */
int cpufreq_set_governor(u32 cpu, cpufreq_governor_t gov);

/** cpufreq_get_governor_name(cpu) — Return string representation of active governor. */
const char *cpufreq_get_governor_name(u32 cpu);
