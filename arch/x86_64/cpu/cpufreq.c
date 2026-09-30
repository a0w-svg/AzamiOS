/* ============================================================================
 * AzamiOS — CPU Frequency Scaling & Hardware P-State (cpufreq) Implementation
 * File: arch/x86_64/cpu/cpufreq.c
 *
 * Drives Intel Speed Shift (HWP) and AMD CPPC hardware autonomous performance
 * control, optimizing CPU frequency transitions at sub-millisecond hardware
 * intervals. Reads APERF / MPERF hardware frequency counters for accurate
 * real-time frequency measurement.
 * ============================================================================ */

#define DEBUG 1
#include "../../../include/azami/debug.h"
#include "cpufreq.h"
#include "msr.h"
#include "smp.h"
#include "../../../kernel/lib/string.h"
#include "../../../kernel/time/timekeeping.h"

static cpu_perf_state_t g_cpu_perf[SMP_MAX_CPUS];
static bool g_cpufreq_initialized = false;

static inline void x86_cpuid(u32 leaf, u32 *eax, u32 *ebx, u32 *ecx, u32 *edx)
{
    __asm__ volatile("cpuid"
                     : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                     : "a"(leaf), "c"(0));
}

static void cpufreq_update_hwp_request(u32 cpu)
{
    cpu_perf_state_t *st = &g_cpu_perf[cpu];
    if (!st->hwp_supported) return;

    u8 min_perf, max_perf, des_perf, epp;

    switch (st->governor) {
    case CPUFREQ_GOV_PERFORMANCE:
        min_perf = st->guaranteed_perf;
        max_perf = st->highest_perf;
        des_perf = 0; /* Autonomous */
        epp      = HWP_EPP_PERFORMANCE;
        break;
    case CPUFREQ_GOV_POWERSAVE:
        min_perf = st->lowest_perf;
        max_perf = st->efficient_perf ? st->efficient_perf : (st->highest_perf / 2);
        des_perf = 0;
        epp      = HWP_EPP_POWERSAVE;
        break;
    case CPUFREQ_GOV_ONDEMAND:
    default:
        min_perf = st->lowest_perf;
        max_perf = st->highest_perf;
        des_perf = 0;
        epp      = HWP_EPP_BALANCE_PERF;
        break;
    }

    u64 req = ((u64)min_perf) |
              ((u64)max_perf << 8) |
              ((u64)des_perf << 16) |
              ((u64)epp << 24);

    wrmsr_safe(MSR_IA32_HWP_REQUEST, req);

    /* Update Energy Performance Bias (EPB) */
    u8 epb_val = (st->governor == CPUFREQ_GOV_PERFORMANCE) ? 0 :
                 (st->governor == CPUFREQ_GOV_POWERSAVE)   ? 15 : 6;
    wrmsr_safe(MSR_IA32_ENERGY_PERF_BIAS, epb_val);
}

void cpufreq_init(void)
{
    if (g_cpufreq_initialized) return;

    u32 max_leaf = 0, b = 0, c = 0, d = 0;
    x86_cpuid(0, &max_leaf, &b, &c, &d);

    bool is_intel = (b == 0x756e6547 && d == 0x49656e69 && c == 0x6c65746e);
    bool is_amd   = (b == 0x68747541 && d == 0x69746e65 && c == 0x444d4163);

    /* 1. Detect base frequency via CPUID Leaf 0x16 (Intel Skylake+) */
    u32 base_mhz = 2400; /* Safe default 2.4 GHz */
    u32 max_mhz  = 3600; /* Safe default 3.6 GHz */
    u32 min_mhz  = 800;  /* Safe default 800 MHz */

    if (is_intel && max_leaf >= 0x16) {
        u32 eax16 = 0, ebx16 = 0, ecx16 = 0, edx16 = 0;
        x86_cpuid(0x16, &eax16, &ebx16, &ecx16, &edx16);
        if (eax16 > 0) base_mhz = eax16;
        if (ebx16 > 0) max_mhz  = ebx16;
        if (ecx16 > 0) min_mhz  = ecx16;
    }

    /* 2. Check for APERF / MPERF (CPUID Leaf 6 ECX bit 0) */
    bool aperf_mperf = false;
    bool hwp_supported = false;
    if (max_leaf >= 6) {
        u32 eax6 = 0, ebx6 = 0, ecx6 = 0, edx6 = 0;
        x86_cpuid(6, &eax6, &ebx6, &ecx6, &edx6);
        if (ecx6 & (1 << 0)) aperf_mperf = true;
        if (is_intel && (eax6 & (1 << 7))) hwp_supported = true;
    }

    /* 3. Check for AMD CPPC */
    bool cppc_supported = false;
    if (is_amd) {
        u32 max_ext = 0;
        x86_cpuid(0x80000000, &max_ext, &b, &c, &d);
        if (max_ext >= 0x80000008) {
            u32 ebx8 = 0;
            x86_cpuid(0x80000008, &b, &ebx8, &c, &d);
            if (ebx8 & (1 << 9)) cppc_supported = true;
        }
    }

    u32 ncpus = smp_cpu_count();
    if (ncpus == 0) ncpus = 1;

    for (u32 i = 0; i < ncpus && i < SMP_MAX_CPUS; i++) {
        cpu_perf_state_t *st = &g_cpu_perf[i];
        st->base_freq_khz = base_mhz * 1000;
        st->max_freq_khz  = max_mhz * 1000;
        st->min_freq_khz  = min_mhz * 1000;
        st->cur_freq_khz  = base_mhz * 1000;
        st->governor      = CPUFREQ_GOV_ONDEMAND;
        st->aperf_mperf_supported = aperf_mperf;

        if (hwp_supported) {
            st->hwp_supported = true;
            /* Enable HWP on this core */
            wrmsr_safe(MSR_IA32_PM_ENABLE, 1);

            u64 caps = 0;
            rdmsr_safe(MSR_IA32_HWP_CAPABILITIES, &caps);
            st->highest_perf    = (u8)(caps >> 0);
            st->guaranteed_perf = (u8)(caps >> 8);
            st->efficient_perf  = (u8)(caps >> 16);
            st->lowest_perf     = (u8)(caps >> 24);

            cpufreq_update_hwp_request(i);
        } else if (cppc_supported) {
            st->cppc_supported = true;
            wrmsr_safe(MSR_AMD_CPPC_ENABLE, 1);
            u64 caps = 0;
            rdmsr_safe(MSR_AMD_CPPC_CAP1, &caps);
            st->highest_perf    = (u8)(caps >> 24);
            st->guaranteed_perf = (u8)(caps >> 16);
            st->efficient_perf  = (u8)(caps >> 8);
            st->lowest_perf     = (u8)(caps >> 0);
        }

        if (aperf_mperf) {
            rdmsr_safe(MSR_IA32_APERF, &st->last_aperf);
            rdmsr_safe(MSR_IA32_MPERF, &st->last_mperf);
            st->last_sample_ns = ktime_get_ns();
        }
    }

    g_cpufreq_initialized = true;

    pr_debug("[CPUFREQ] Hardware P-State scaling active: %s (Base: %u MHz, Max Turbo: %u MHz)\n",
             hwp_supported ? "Intel HWP (Speed Shift)" :
             cppc_supported ? "AMD CPPC" : "Standard Frequency Scaling",
             base_mhz, max_mhz);
}

u32 cpufreq_get_cur_freq(u32 cpu)
{
    if (cpu >= SMP_MAX_CPUS) return 2400000;
    cpu_perf_state_t *st = &g_cpu_perf[cpu];
    if (!st->base_freq_khz) return 2400000;

    if (st->aperf_mperf_supported) {
        u64 aperf = 0, mperf = 0;
        rdmsr_safe(MSR_IA32_APERF, &aperf);
        rdmsr_safe(MSR_IA32_MPERF, &mperf);
        u64 delta_a = (aperf >= st->last_aperf) ? (aperf - st->last_aperf) : 0;
        u64 delta_m = (mperf >= st->last_mperf) ? (mperf - st->last_mperf) : 0;

        st->last_aperf = aperf;
        st->last_mperf = mperf;

        if (delta_m > 1000) {
            u64 freq = ((u64)st->base_freq_khz * delta_a) / delta_m;
            if (freq < st->min_freq_khz) freq = st->min_freq_khz;
            if (freq > st->max_freq_khz * 2) freq = st->max_freq_khz;
            st->cur_freq_khz = (u32)freq;
        }
    }

    return st->cur_freq_khz;
}

u32 cpufreq_get_max_freq(u32 cpu)
{
    if (cpu >= SMP_MAX_CPUS) return 3600000;
    return g_cpu_perf[cpu].max_freq_khz ? g_cpu_perf[cpu].max_freq_khz : 3600000;
}

u32 cpufreq_get_min_freq(u32 cpu)
{
    if (cpu >= SMP_MAX_CPUS) return 800000;
    return g_cpu_perf[cpu].min_freq_khz ? g_cpu_perf[cpu].min_freq_khz : 800000;
}

int cpufreq_set_governor(u32 cpu, cpufreq_governor_t gov)
{
    if (cpu >= SMP_MAX_CPUS) return -EINVAL;
    g_cpu_perf[cpu].governor = gov;
    cpufreq_update_hwp_request(cpu);
    return 0;
}

const char *cpufreq_get_governor_name(u32 cpu)
{
    if (cpu >= SMP_MAX_CPUS) return "ondemand";
    switch (g_cpu_perf[cpu].governor) {
    case CPUFREQ_GOV_PERFORMANCE: return "performance";
    case CPUFREQ_GOV_POWERSAVE:   return "powersave";
    case CPUFREQ_GOV_ONDEMAND:
    default:                      return "ondemand";
    }
}
