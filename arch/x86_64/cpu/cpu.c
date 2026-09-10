/* ============================================================================
 * AzamiOS — CPU Feature Detection & Extensions Management (x86_64)
 * File: arch/x86_64/cpu/cpu.c
 *
 * Two phases, deliberately separated:
 *   cpu_detect_features()      — pure CPUID enumeration, no architectural state
 *                                is touched. Runs once on the BSP.
 *   cpu_enable_features_bsp()  — turns the detected extensions on (CR0/CR4/
 *   cpu_enable_features_ap()     XCR0/EFER/MSRs) and records what actually
 *                                stuck in the g_*_enabled globals. The AP path
 *                                replays exactly the BSP's decisions so every
 *                                core runs with an identical configuration —
 *                                a core with, say, XCR0 missing the AVX bit
 *                                would #GP the moment a migrated thread's
 *                                XRSTOR named it.
 * ============================================================================ */

#include "cpu.h"
#include "hwaccel.h"
#include "mce.h"
#include "mitigations.h"
#include "msr.h"
#include "../../../kernel/lib/string.h"

extern void kprintf(const char *fmt, ...);
extern int  scnprintf(char *buf, size_t size, const char *fmt, ...);

cpu_features_t g_cpu_info;

/* Global enablement flags */
u8  g_fsgsbase_enabled = 0;
u8  g_smep_enabled     = 0;
u8  g_smap_enabled     = 0;
u8  g_osxsave_enabled  = 0;
u8  g_pge_enabled      = 0;
u8  g_umip_enabled     = 0;
u8  g_pku_enabled      = 0;
u8  g_pcid_enabled     = 0;
u8  g_invpcid_enabled  = 0;
u8  g_erms_enabled     = 0;
u8  g_mwait_idle_enabled = 0;
u8  g_xsave_variant    = XSAVE_VARIANT_FXSAVE;
u32 g_xsave_area_size  = 512;
u64 g_xcr0_mask        = 0;
u8  g_split_lock_detect  = 0;
u64 g_cr4_pinned         = 0;

/* MWAIT hint for the idle loop: C-state 0 (C1), sub-state 0 — the one every
 * implementation understands. We deliberately do not use the ECX=1
 * "interrupts break the wait even when masked" extension: leaf 5 advertises it
 * on hosts that then #GP on it, and the plain STI;MWAIT sequence is race-free
 * on its own (STI's shadow covers the MWAIT). */
static u32 s_mwait_hint = 0;

bool cpu_is_intel(void) { return strcmp(g_cpu_info.vendor_id, "GenuineIntel") == 0; }
bool cpu_is_amd(void)   { return strcmp(g_cpu_info.vendor_id, "AuthenticAMD") == 0 ||
                                 strcmp(g_cpu_info.vendor_id, "HygonGenuine") == 0; }

/* ── Hardware entropy ────────────────────────────────────────────────────── */

bool cpu_rand64(u64 *out)
{
    if (!out) return false;

    /* RDSEED is the conditioned entropy sample and RDRAND the DRBG downstream
     * of it, so RDSEED first. Ten attempts each: Intel documents RDSEED as
     * needing "at most" a handful of retries under contention, and an
     * unbounded loop here would hang the boot on a part whose entropy source
     * has genuinely failed. */
    if (g_cpu_info.has_rdseed) {
        for (int i = 0; i < 10; i++)
            if (rdseed64_step(out) && *out != 0) return true;
    }
    if (g_cpu_info.has_rdrand) {
        for (int i = 0; i < 10; i++)
            if (rdrand64_step(out) && *out != 0) return true;
    }
    return false;
}

/* ── Detection ───────────────────────────────────────────────────────────── */

/* Decode one CPUID leaf-4 cache descriptor into the per-level size fields. */
static void decode_cache_leaf(u32 eax, u32 ebx, u32 ecx)
{
    u32 type  = eax & 0x1F;
    if (type == 0) return;                       /* no more caches */
    u32 level = (eax >> 5) & 0x7;

    u32 ways       = ((ebx >> 22) & 0x3FF) + 1;
    u32 partitions = ((ebx >> 12) & 0x3FF) + 1;
    u32 line_size  = (ebx & 0xFFF) + 1;
    u32 sets       = ecx + 1;
    u32 size_kb    = (ways * partitions * line_size * sets) / 1024;

    switch (level) {
        case 1: if (type == 1) g_cpu_info.cache_l1d_kb = size_kb;   /* data        */
                else           g_cpu_info.cache_l1i_kb = size_kb;   /* instruction */
                break;
        case 2: g_cpu_info.cache_l2_kb = size_kb; break;
        case 3: g_cpu_info.cache_l3_kb = size_kb; break;
        default: break;
    }
}

void cpu_detect_features(void)
{
    memset(&g_cpu_info, 0, sizeof(g_cpu_info));

    /* ── Leaf 0: Max standard leaf and Vendor String ──────────────────────── */
    u32 eax = 0, ebx = 0, ecx = 0, edx = 0;
    cpuid(0, 0, &eax, &ebx, &ecx, &edx);
    g_cpu_info.max_leaf = eax;

    /* Vendor string in EBX, EDX, ECX order */
    *(u32 *)&g_cpu_info.vendor_id[0] = ebx;
    *(u32 *)&g_cpu_info.vendor_id[4] = edx;
    *(u32 *)&g_cpu_info.vendor_id[8] = ecx;
    g_cpu_info.vendor_id[12] = '\0';

    /* ── Leaf 1: Family/Model/Stepping & Standard Features ────────────────── */
    if (g_cpu_info.max_leaf >= 1) {
        cpuid(1, 0, &eax, &ebx, &ecx, &edx);
        g_cpu_info.stepping = eax & 0xF;
        g_cpu_info.model    = ((eax >> 4) & 0xF) | (((eax >> 16) & 0xF) << 4);
        g_cpu_info.family   = ((eax >> 8) & 0xF) + ((eax >> 20) & 0xFF);
        g_cpu_info.clflush_size = ((ebx >> 8) & 0xFF) * 8;
        if (g_cpu_info.clflush_size == 0) g_cpu_info.clflush_size = 64;

        g_cpu_info.features = ((u64)ecx << 32) | (u64)edx;

        if (edx & (1U << 13)) g_cpu_info.has_pge = true;
        if (ecx & (1U << 28)) g_cpu_info.has_avx = true;
        if (ecx & (1U << 3))  g_cpu_info.has_monitor      = true;
        if (ecx & (1U << 17)) g_cpu_info.has_pcid         = true;
        if (ecx & (1U << 21)) g_cpu_info.has_x2apic       = true;
        if (ecx & (1U << 24)) g_cpu_info.has_tsc_deadline = true;
        if (ecx & (1U << 20)) g_cpu_info.has_sse4_2       = true;
        if (ecx & (1U << 23)) g_cpu_info.has_popcnt       = true;
        if (ecx & (1U << 30)) g_cpu_info.has_rdrand       = true;
        if (edx & (1U << 7))  g_cpu_info.has_mce          = true;
        if (edx & (1U << 14)) g_cpu_info.has_mca          = true;
    }

    /* ── Leaf 4: Deterministic cache parameters (Intel) ───────────────────── */
    if (g_cpu_info.max_leaf >= 4) {
        for (u32 sub = 0; sub < 8; sub++) {
            cpuid(4, sub, &eax, &ebx, &ecx, &edx);
            if ((eax & 0x1F) == 0) break;
            if (sub == 0) g_cpu_info.cores_per_package = ((eax >> 26) & 0x3F) + 1;
            decode_cache_leaf(eax, ebx, ecx);
        }
    }

    /* ── Leaf 5: MONITOR/MWAIT parameters ─────────────────────────────────── */
    if (g_cpu_info.max_leaf >= 5) {
        cpuid(5, 0, &eax, &ebx, &ecx, &edx);
        /* EDX nibble 0 counts the C1 sub-states MWAIT understands. */
        g_cpu_info.mwait_c1_substates = (edx >> 4) & 0xF;
    }

    /* ── Leaf 6: Thermal & power management ───────────────────────────────── */
    if (g_cpu_info.max_leaf >= 6) {
        cpuid(6, 0, &eax, &ebx, &ecx, &edx);
        if (eax & (1U << 2)) g_cpu_info.has_arat = true;  /* always-running APIC timer */
    }

    /* ── Leaf 7: Structured Extended Features ─────────────────────────────── */
    if (g_cpu_info.max_leaf >= 7) {
        cpuid(7, 0, &eax, &ebx, &ecx, &edx);
        u32 max_subleaf = eax;
        g_cpu_info.ext_features  = ((u64)ecx << 32) | (u64)ebx;
        g_cpu_info.extd_features = (u64)edx;

        if (ebx & (1U << 0))  g_cpu_info.has_fsgsbase = true;
        if (ebx & (1U << 5))  g_cpu_info.has_avx2     = true;
        if (ebx & (1U << 7))  g_cpu_info.has_smep     = true;
        if (ebx & (1U << 9))  g_cpu_info.has_erms     = true;
        if (ebx & (1U << 10)) g_cpu_info.has_invpcid  = true;
        if (ebx & (1U << 16)) g_cpu_info.has_avx512f  = true;
        if (ebx & (1U << 20)) g_cpu_info.has_smap     = true;
        if (ecx & (1U << 2))  g_cpu_info.has_umip     = true;
        if (ecx & (1U << 3))  g_cpu_info.has_pku      = true;
        if (ecx & (1U << 22)) g_cpu_info.has_rdpid    = true;
        if (edx & (1U << 4))  g_cpu_info.has_fsrm     = true;

        if (ebx & (1U << 3))  g_cpu_info.has_bmi1       = true;
        if (ebx & (1U << 8))  g_cpu_info.has_bmi2       = true;
        if (ebx & (1U << 18)) g_cpu_info.has_rdseed     = true;
        if (ebx & (1U << 23)) g_cpu_info.has_clflushopt = true;
        if (ebx & (1U << 24)) g_cpu_info.has_clwb       = true;
        if (ecx & (1U << 5))  g_cpu_info.has_waitpkg    = true;
        if (ecx & (1U << 7))  g_cpu_info.has_shstk      = true;
        if (ecx & (1U << 25)) g_cpu_info.has_cldemote   = true;
        if (ecx & (1U << 27)) g_cpu_info.has_movdiri    = true;
        if (ecx & (1U << 28)) g_cpu_info.has_movdir64b  = true;
        if (edx & (1U << 14)) g_cpu_info.has_serialize  = true;
        if (edx & (1U << 20)) g_cpu_info.has_ibt        = true;

        if (ecx & (1U << 16)) g_cpu_info.has_la57       = true;
        if (ecx & (1U << 23)) g_cpu_info.has_keylocker  = true;
        if (ecx & (1U << 31)) g_cpu_info.has_pks        = true;
        if (edx & (1U << 10)) g_cpu_info.has_md_clear   = true;
        if (edx & (1U << 15)) g_cpu_info.has_hybrid     = true;
        if (edx & (1U << 24)) g_cpu_info.has_amx        = true;
        if (edx & (1U << 27)) g_cpu_info.has_stibp      = true;
        if (edx & (1U << 28)) g_cpu_info.has_l1d_flush  = true;
        if (edx & (1U << 30)) g_cpu_info.has_core_caps  = true;
        if (edx & (1U << 31)) g_cpu_info.has_ssbd       = true;

        /* IA32_PRED_CMD (IBPB) rides along with the IBRS enumeration on Intel;
         * AMD gives it its own bit in leaf 0x80000008, folded in below. */
        if (edx & (1U << 26)) g_cpu_info.has_ibpb       = true;

        /* TZCNT/LZCNT decode as BSF/BSR on parts without BMI1/ABM, which gives
         * the *wrong* answer for a zero input rather than faulting — so they
         * are only safe to emit once one of those bits is actually set. */
        if (g_cpu_info.has_bmi1) g_cpu_info.has_lzcnt = true;

        if (max_subleaf >= 1) {
            cpuid(7, 1, &eax, &ebx, &ecx, &edx);
            g_cpu_info.ext1_features  = (u64)eax;
            g_cpu_info.ext1d_features = (u64)edx;
            if (eax & (1U << 26)) g_cpu_info.has_lam = true;
        }

        /* Subleaf 2 carries the second-generation branch-prediction controls
         * (IPRED/RRSBA/BHI). They are the difference between "we can turn IBRS
         * on wholesale" and "we can fence off exactly the predictor this part
         * is affected through", so the mitigation engine wants them by name. */
        if (max_subleaf >= 2) {
            cpuid(7, 2, &eax, &ebx, &ecx, &edx);
            g_cpu_info.ext2d_features = (u64)edx;
        }
    }

    /* ── Leaf 0xD: XSAVE Feature Details ──────────────────────────────────── */
    if (g_cpu_info.max_leaf >= 0xD) {
        cpuid(0xD, 0, &eax, &ebx, &ecx, &edx);
        g_cpu_info.xsave_supported_mask = ((u64)edx << 32) | (u64)eax;
        g_cpu_info.xsave_max_size = ecx;

        cpuid(0xD, 1, &eax, &ebx, &ecx, &edx);
        if (eax & (1U << 0)) g_cpu_info.has_xsaveopt = true;
        if (eax & (1U << 1)) g_cpu_info.has_xgetbv1  = true;
        if (eax & (1U << 2)) g_cpu_info.has_xsavec   = true;
        if (eax & (1U << 3)) g_cpu_info.has_xsaves   = true;
    }

    /* ── Leaf 0xB: SMT topology ───────────────────────────────────────────── */
    if (g_cpu_info.max_leaf >= 0xB) {
        cpuid(0xB, 0, &eax, &ebx, &ecx, &edx);
        if ((ebx & 0xFFFF) != 0) g_cpu_info.threads_per_core = ebx & 0xFFFF;
    }

    /* ── Leaf 0x15/0x16: TSC and core frequencies ─────────────────────────── */
    if (g_cpu_info.max_leaf >= 0x15) {
        cpuid(0x15, 0, &eax, &ebx, &ecx, &edx);
        /* TSC = core-crystal * EBX/EAX. ECX carries the crystal Hz when known. */
        if (eax && ebx && ecx)
            g_cpu_info.tsc_khz = (u32)(((u64)ecx / 1000ULL) * ebx / eax);
    }
    if (g_cpu_info.max_leaf >= 0x16) {
        cpuid(0x16, 0, &eax, &ebx, &ecx, &edx);
        g_cpu_info.base_mhz = eax & 0xFFFF;
        g_cpu_info.max_mhz  = ebx & 0xFFFF;
        g_cpu_info.bus_mhz  = ecx & 0xFFFF;
        if (g_cpu_info.tsc_khz == 0 && g_cpu_info.base_mhz)
            g_cpu_info.tsc_khz = g_cpu_info.base_mhz * 1000;
    }

    /* ── Leaf 0x80000000: Max Extended Function Leaf ──────────────────────── */
    cpuid(0x80000000, 0, &eax, &ebx, &ecx, &edx);
    g_cpu_info.max_ext_leaf = eax;

    /* ── Leaf 0x80000001: Extended Processor Info ─────────────────────────── */
    if (g_cpu_info.max_ext_leaf >= 0x80000001) {
        cpuid(0x80000001, 0, &eax, &ebx, &ecx, &edx);
        g_cpu_info.ext2_features = ((u64)ecx << 32) | (u64)edx;
        if (edx & (1U << 26)) g_cpu_info.has_1gb_pages = true;
        if (edx & (1U << 27)) g_cpu_info.has_rdtscp    = true;
        if (edx & (1U << 29)) g_cpu_info.has_lzcnt     = true;  /* AMD LM/ABM  */
        if (ecx & (1U << 5))  g_cpu_info.has_lzcnt     = true;  /* ABM         */
        if (ecx & (1U << 8))  g_cpu_info.has_prefetchw = true;
    }

    /* ── Leaves 0x80000002..4: Processor Brand String ─────────────────────── */
    if (g_cpu_info.max_ext_leaf >= 0x80000004) {
        u32 *brand_ptr = (u32 *)g_cpu_info.brand_string;
        for (u32 leaf = 0x80000002; leaf <= 0x80000004; leaf++) {
            cpuid(leaf, 0, &brand_ptr[0], &brand_ptr[1], &brand_ptr[2], &brand_ptr[3]);
            brand_ptr += 4;
        }
        g_cpu_info.brand_string[48] = '\0';

        /* Strip leading whitespace */
        char *p = g_cpu_info.brand_string;
        while (*p == ' ') p++;
        if (p != g_cpu_info.brand_string) {
            memmove(g_cpu_info.brand_string, p, strlen(p) + 1);
        }
    } else {
        strncpy(g_cpu_info.brand_string, "x86_64 Processor", sizeof(g_cpu_info.brand_string) - 1);
    }

    /* ── Leaf 0x80000005/6: AMD cache sizes (leaf 4 is Intel-only) ────────── */
    if (g_cpu_info.max_ext_leaf >= 0x80000006 && g_cpu_info.cache_l2_kb == 0) {
        cpuid(0x80000005, 0, &eax, &ebx, &ecx, &edx);
        if (g_cpu_info.cache_l1d_kb == 0) g_cpu_info.cache_l1d_kb = (ecx >> 24) & 0xFF;
        if (g_cpu_info.cache_l1i_kb == 0) g_cpu_info.cache_l1i_kb = (edx >> 24) & 0xFF;
        cpuid(0x80000006, 0, &eax, &ebx, &ecx, &edx);
        g_cpu_info.cache_l2_kb = (ecx >> 16) & 0xFFFF;
        g_cpu_info.cache_l3_kb = ((edx >> 18) & 0x3FFF) * 512;
    }

    /* ── Leaf 0x80000007: invariant TSC ───────────────────────────────────── */
    if (g_cpu_info.max_ext_leaf >= 0x80000007) {
        cpuid(0x80000007, 0, &eax, &ebx, &ecx, &edx);
        g_cpu_info.pm_features = (u64)edx;
        if (edx & (1U << 8)) g_cpu_info.has_invariant_tsc = true;
    }

    /* ── Leaf 0x80000008: physical/linear address widths ──────────────────── */
    if (g_cpu_info.max_ext_leaf >= 0x80000008) {
        cpuid(0x80000008, 0, &eax, &ebx, &ecx, &edx);
        g_cpu_info.phys_addr_bits = eax & 0xFF;
        g_cpu_info.virt_addr_bits = (eax >> 8) & 0xFF;
        g_cpu_info.ext3_features  = (u64)ebx;
        if (ebx & (1U << 0))  g_cpu_info.has_clzero   = true;
        if (ebx & (1U << 9))  g_cpu_info.has_wbnoinvd = true;
        if (ebx & (1U << 12)) g_cpu_info.has_ibpb     = true;
        if (ebx & (1U << 15)) g_cpu_info.has_stibp    = true;
        if (ebx & (1U << 24)) g_cpu_info.has_ssbd     = true;
    }

    /* ── Leaf 0x80000021: AMD extended features 2 ──────────────────────────
     * AutoIBRS lives here, and it is the cheapest Spectre-v2 mitigation any
     * part offers: one EFER bit instead of an MSR write per privilege change. */
    if (g_cpu_info.max_ext_leaf >= 0x80000021) {
        cpuid(0x80000021, 0, &eax, &ebx, &ecx, &edx);
        g_cpu_info.amd2_features = (u64)eax;
    }
    if (g_cpu_info.phys_addr_bits == 0) g_cpu_info.phys_addr_bits = 40;
    if (g_cpu_info.virt_addr_bits == 0) g_cpu_info.virt_addr_bits = 48;

    /* ── IA32_ARCH_CAPABILITIES: which speculation bugs we are immune to ──── */
    if (g_cpu_info.extd_features & CPU_EXTD_ARCH_CAPS) {
        g_cpu_info.arch_caps = rdmsr(MSR_IA32_ARCH_CAPABILITIES);
    }

    /* ── IA32_CORE_CAPABILITIES: per-core knobs, currently split-lock ─────── */
    if (g_cpu_info.has_core_caps) {
        g_cpu_info.core_caps = rdmsr(MSR_IA32_CORE_CAPABILITIES);
        if (g_cpu_info.core_caps & CORE_CAP_SPLIT_LOCK_DETECT)
            g_cpu_info.has_split_lock_detect = true;
    }
    if (g_cpu_info.ext_features & CPU_EXT_BUS_LOCK_DETECT)
        g_cpu_info.has_bus_lock_detect = true;

    /* ── IA32_MCG_CAP: how many machine-check banks this part reports ─────── *
     * Only readable once both the #MC exception and the MCA bank MSRs are
     * enumerated; a part with MCE but not MCA has the exception and no banks
     * to decode it from, which mce.c handles as a bankless machine check. */
    if (g_cpu_info.has_mce && g_cpu_info.has_mca) {
        g_cpu_info.mcg_cap   = rdmsr(MSR_IA32_MCG_CAP);
        g_cpu_info.mce_banks = (u32)(g_cpu_info.mcg_cap & MCG_CAP_COUNT_MASK);
    }

    kprintf("[CPU] Vendor: %s, Model: %s\n", g_cpu_info.vendor_id, g_cpu_info.brand_string);
    kprintf("[CPU] Family: %u, Model: %u, Stepping: %u, CacheLine: %u bytes\n",
            g_cpu_info.family, g_cpu_info.model, g_cpu_info.stepping, g_cpu_info.clflush_size);
    kprintf("[CPU] Address bits: %u phys / %u virt; caches: L1d %u KB, L1i %u KB, "
            "L2 %u KB, L3 %u KB\n",
            g_cpu_info.phys_addr_bits, g_cpu_info.virt_addr_bits,
            g_cpu_info.cache_l1d_kb, g_cpu_info.cache_l1i_kb,
            g_cpu_info.cache_l2_kb, g_cpu_info.cache_l3_kb);
    if (g_cpu_info.tsc_khz) {
        kprintf("[CPU] TSC %u.%03u MHz%s\n",
                g_cpu_info.tsc_khz / 1000, g_cpu_info.tsc_khz % 1000,
                g_cpu_info.has_invariant_tsc ? " (invariant)" : "");
    }
}

/* ── Enablement ──────────────────────────────────────────────────────────── */

/* The CR0/CR4 bit names live in msr.h so this file, the mitigation engine and
 * the control-register pinning audit cannot drift apart on which bit is which. */

/* PKRU value a fresh context starts from: every key but 0 denied, matching
 * Linux's init_pkru. Key 0 is what every page carries until pkey_mprotect()
 * tags it, so this changes nothing for code that never calls pkey_alloc(). */
#define PKRU_INIT      0x55555554u

/* x87+SSE are mandatory for XSAVE to be usable at all; the rest are optional
 * state components we add when both the CPU and our fixed-size per-thread save
 * area can take them. */
#define XCR0_X87_SSE   0x0003ULL
#define XCR0_AVX       0x0004ULL
#define XCR0_AVX512    0x00E0ULL   /* opmask + ZMM_Hi256 + Hi16_ZMM, all-or-nothing */
#define XCR0_PKRU      0x0200ULL

/* Turn the CPU's floating-point unit on and take the OS's half of the FPU
 * contract: MP set (so a #NM is cooperative), EM and TS clear (no emulation,
 * no lazy-switch traps — the scheduler saves the state eagerly), and WP set so
 * ring 0 honours read-only PTEs instead of silently writing through them. */
static void apply_cr0(void)
{
    u64 cr0 = read_cr0();
    cr0 &= ~(CR0_EM | CR0_TS);
    cr0 |= CR0_MP | CR0_WP;
    write_cr0(cr0);
}

/* Compose the CR4 value implied by the g_*_enabled decisions. Shared by the
 * BSP (which sets those flags) and the APs (which only replay them). */
static u64 compose_cr4(u64 cr4)
{
    cr4 |= CR4_OSFXSR | CR4_OSXMMEXCPT;
    if (g_pge_enabled)      cr4 |= CR4_PGE;
    if (g_umip_enabled)     cr4 |= CR4_UMIP;
    if (g_fsgsbase_enabled) cr4 |= CR4_FSGSBASE;
    if (g_osxsave_enabled)  cr4 |= CR4_OSXSAVE;
    if (g_smep_enabled)     cr4 |= CR4_SMEP;
    if (g_smap_enabled)     cr4 |= CR4_SMAP;
    if (g_pku_enabled)      cr4 |= CR4_PKE;
    /* PCID tags TLB entries by address space, so a context switch keeps the
     * incoming process's translations instead of flushing the whole
     * non-global TLB. Setting the bit is only legal while CR3[11:0] == 0,
     * which holds here — PCID has never been enabled, so CR3 is a bare PML4. */
    if (g_pcid_enabled)     cr4 |= CR4_PCIDE;

    /* RDPMC from ring 3 is a side channel with no user in this kernel: it hands
     * userspace cycle-accurate counters for code it does not own. Never set. */
    cr4 &= ~CR4_PCE;
    return cr4;
}

/* ── Control-register pinning ─────────────────────────────────────────────── *
 * SMEP, SMAP, UMIP and FSGSBASE are decided once at boot and are never
 * legitimately turned off again. Pinning them costs one OR per CR4 write and
 * removes a whole class of exploit step: a write primitive aimed at CR4 (the
 * classic prelude to running a ret2usr payload with SMEP disabled) no longer
 * has an effect, because the value that reaches the register always has the
 * protection bits set. PGE is deliberately *not* pinned — tlb.c toggles it to
 * force a global TLB flush, which is a legitimate transient clear.
 * ------------------------------------------------------------------------- */
#define CR4_PINNABLE  (CR4_SMEP | CR4_SMAP | CR4_UMIP | CR4_FSGSBASE)

void cpu_write_cr4(u64 val)
{
    /* PCE is pinned *clear* rather than set, so it needs its own mask. */
    u64 want = (val | g_cr4_pinned) & ~CR4_PCE;

    if (want != val) {
        /* A value missing a pinned bit is only suspicious once that bit was
         * actually live on this core: an AP raising CR4 from its reset value
         * legitimately passes through a state that has none of them yet. Only
         * a write that would *clear* a set protection bit is worth a line in
         * the log — that is the shape a disarm attempt takes. */
        u64 live = read_cr4() & g_cr4_pinned;
        if (live & ~val) {
            kprintf("[CPU] CR4 write would clear live pinned bits 0x%llx; "
                    "restoring\n", (unsigned long long)(live & ~val));
        }
    }
    write_cr4(want);
}

void cpu_pin_control_regs(void)
{
    g_cr4_pinned = read_cr4() & CR4_PINNABLE;
}

bool cpu_check_control_regs(const char *who)
{
    bool ok = true;

    u64 cr0 = read_cr0();
    if (!(cr0 & CR0_WP)) {
        /* Without WP, ring 0 writes straight through read-only PTEs — kernel
         * .rodata and every page table become writable from any kernel bug. */
        kprintf("[CPU] %s: CR0.WP was clear; re-arming\n", who ? who : "audit");
        write_cr0(cr0 | CR0_WP);
        ok = false;
    }

    u64 cr4 = read_cr4();
    if ((cr4 & g_cr4_pinned) != g_cr4_pinned || (cr4 & CR4_PCE)) {
        kprintf("[CPU] %s: CR4=0x%llx violates pin mask 0x%llx; repairing\n",
                who ? who : "audit", (unsigned long long)cr4,
                (unsigned long long)g_cr4_pinned);
        cpu_write_cr4(cr4);
        ok = false;
    }
    return ok;
}

/* Size of the XSAVE image the CPU will write for the currently programmed
 * XCR0, in the layout our save variant uses. */
static u32 xsave_area_size_for_current_xcr0(bool compacted)
{
    u32 a, b, c, d;
    cpuid(0xD, compacted ? 1 : 0, &a, &b, &c, &d);
    return b;
}

/* Scratch for the instruction probe below. Static rather than stack-allocated:
 * this runs from vmm_init() on the boot stack, and FPU_STATE_MAX_SIZE is more
 * than that path should be spending. */
static u8 s_xsave_probe_area[FPU_STATE_MAX_SIZE] __attribute__((aligned(64)));

extern int xsave_probe_asm(void *area, u64 mask, u32 variant);

/* Pick the strongest save instruction that actually runs on this CPU.
 *
 * CPUID leaf 0xD advertising XSAVEC is not proof it is implemented — QEMU's TCG
 * mode sets the bit and then raises #UD on the instruction, which took the
 * whole kernel down on every core the first time a thread was switched out. The
 * probe executes each candidate once against a scratch area, under an .extable
 * fixup, and steps down until one survives. */
static void select_xsave_variant(void)
{
    u8 variant = g_cpu_info.has_xsavec   ? XSAVE_VARIANT_XSAVEC
               : g_cpu_info.has_xsaveopt ? XSAVE_VARIANT_XSAVEOPT
                                         : XSAVE_VARIANT_XSAVE;

    for (;;) {
        memset(s_xsave_probe_area, 0, sizeof s_xsave_probe_area);
        if (xsave_probe_asm(s_xsave_probe_area, g_xcr0_mask, variant) == 0) {
            g_xsave_variant = variant;
            return;
        }
        kprintf("[CPU] %s advertised by CPUID but raised #UD; stepping down\n",
                variant == XSAVE_VARIANT_XSAVEC   ? "XSAVEC" :
                variant == XSAVE_VARIANT_XSAVEOPT ? "XSAVEOPT" : "XSAVE");
        if (variant == XSAVE_VARIANT_XSAVE) {
            /* Not even plain XSAVE works: fall back to FXSAVE wholesale. */
            g_osxsave_enabled = 0;
            g_xcr0_mask = 0;
            g_xsave_variant = XSAVE_VARIANT_FXSAVE;
            g_xsave_area_size = 512;
            g_cpu_info.xcr0_active_mask = 0;
            cpu_write_cr4(read_cr4() & ~CR4_OSXSAVE);
            return;
        }
        variant--;
    }
}

/* Program XCR0, backing off the optional components until the resulting save
 * area fits FPU_STATE_MAX_SIZE. A CPU whose AVX-512 image overflows our
 * per-thread buffer runs with AVX only rather than corrupting the next
 * thread's control block — an OS that enables a component it cannot save is
 * strictly worse than one that leaves it off. */
static void configure_xsave(void)
{
    if (!(g_cpu_info.features & CPU_FEAT_XSAVE)) return;
    if ((g_cpu_info.xsave_supported_mask & XCR0_X87_SSE) != XCR0_X87_SSE) return;

    u64 want = XCR0_X87_SSE;
    if (g_cpu_info.has_avx)                        want |= XCR0_AVX;
    if (g_cpu_info.has_avx512f && (want & XCR0_AVX)) want |= XCR0_AVX512;
    if (g_cpu_info.has_pku)                        want |= XCR0_PKRU;
    want &= g_cpu_info.xsave_supported_mask;

    /* AVX-512 needs all three of its components or none. */
    if ((want & XCR0_AVX512) != XCR0_AVX512) want &= ~XCR0_AVX512;
    if (!(want & XCR0_AVX))                  want &= ~XCR0_AVX512;

    /* OSXSAVE must be live before XSETBV, and CPUID leaf 0xD reports sizes
     * against the XCR0 that is actually programmed. */
    cpu_write_cr4(read_cr4() | CR4_OSXSAVE);

    /* Size against the *standard* layout throughout: it is the largest of the
     * three and XSAVEC's compacted image can only be smaller, so a budget that
     * fits this fits whichever variant the probe ends up choosing. */
    for (;;) {
        xsetbv(0, want);
        u32 size = xsave_area_size_for_current_xcr0(false);
        if (size <= FPU_STATE_MAX_SIZE - 63) {
            g_xsave_area_size = size;
            break;
        }
        if (want & XCR0_AVX512)      { want &= ~XCR0_AVX512; continue; }
        if (want & XCR0_PKRU)        { want &= ~XCR0_PKRU;   continue; }
        if (want & XCR0_AVX)         { want &= ~XCR0_AVX;    continue; }
        /* Even x87+SSE does not fit: XSAVE is unusable, fall back to FXSAVE. */
        xsetbv(0, XCR0_X87_SSE);
        cpu_write_cr4(read_cr4() & ~CR4_OSXSAVE);
        return;
    }

    g_osxsave_enabled = 1;
    g_xcr0_mask = want;
    g_cpu_info.xcr0_active_mask = want;
    g_cpu_info.xsave_area_size  = g_xsave_area_size;

    /* We save no supervisor state, so XSAVES/XRSTORS would have nothing extra
     * to do; keep IA32_XSS clear so a compacted image stays user-only. */
    if (g_cpu_info.has_xsaves) wrmsr(MSR_IA32_XSS, 0);

    select_xsave_variant();
    if (!g_osxsave_enabled) {
        kprintf("[CPU] XSAVE unusable on this CPU; using FXSAVE (x87+SSE only)\n");
        return;
    }

    kprintf("[CPU] XSAVE: XCR0=0x%llx, area=%u bytes, variant=%s%s\n",
            (unsigned long long)g_xcr0_mask, g_xsave_area_size,
            g_xsave_variant == XSAVE_VARIANT_XSAVEC   ? "XSAVEC" :
            g_xsave_variant == XSAVE_VARIANT_XSAVEOPT ? "XSAVEOPT" : "XSAVE",
            (g_xcr0_mask & XCR0_AVX512) ? " (+AVX-512)" :
            (g_xcr0_mask & XCR0_AVX)    ? " (+AVX)" : "");
}

void cpu_enable_features_bsp(void)
{
    apply_cr0();

    /* Record what we intend to turn on, then let compose_cr4() build the value;
     * the AP path replays the same flags so all cores agree. */
    if (g_cpu_info.has_pge)      g_pge_enabled      = 1;
    if (g_cpu_info.has_umip)     g_umip_enabled     = 1;
    if (g_cpu_info.has_fsgsbase) g_fsgsbase_enabled = 1;
    if (g_cpu_info.has_smep)     g_smep_enabled     = 1;
    if (g_cpu_info.has_smap)     g_smap_enabled     = 1;
    if (g_cpu_info.has_pku)      g_pku_enabled      = 1;
    if (g_cpu_info.has_erms)     g_erms_enabled     = 1;
    if (g_cpu_info.has_invpcid)  g_invpcid_enabled  = 1;
    /* Require INVPCID alongside PCID: the TLB-shootdown handler leans on
     * INVPCID "all contexts, including globals" to flush every tagged context
     * on a remote core in one instruction. Both are present on every CPU new
     * enough to care and on QEMU's -cpu max. */
    if (g_cpu_info.has_pcid && g_cpu_info.has_invpcid) g_pcid_enabled = 1;

    /* XSAVE first: configure_xsave() needs OSXSAVE live and may decide the
     * feature is unusable, which compose_cr4() then has to reflect. */
    configure_xsave();
    cpu_write_cr4(compose_cr4(read_cr4()));

    /* PKRU is only addressable once CR4.PKE is set. */
    if (g_pku_enabled) wrpkru(PKRU_INIT);

    /* No-Execute: the VMM maps every data and stack page NX. */
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_NXE);

    /* RDTSCP/RDPID hand userspace this MSR's low 32 bits as the CPU id; the
     * SMP layer overwrites it per-core in cpu_enable_features_ap(). */
    if (g_cpu_info.has_rdtscp || g_cpu_info.has_rdpid) wrmsr(MSR_TSC_AUX, 0);


    /* MWAIT lets an idle core drop into a low-power state and wake on a plain
     * store to the monitored line, so waking it needs no IPI at all. As with
     * XSAVEC, the CPUID bit is only a hint — probe before relying on it. */
    if (g_cpu_info.has_monitor) {
        extern int mwait_probe_asm(void *scratch);
        s_mwait_hint = 0;   /* C1, sub-state 0 — universally supported */
        if (mwait_probe_asm(s_xsave_probe_area) == 0) {
            g_mwait_idle_enabled = 1;
        } else {
            kprintf("[CPU] MONITOR/MWAIT advertised by CPUID but faulted; "
                    "idling with HLT\n");
        }
    }

    /* Everything CR4 is ever going to carry is in place now, so freeze the
     * protection bits before any code that is not this function runs. */
    cpu_pin_control_regs();

    /* Speculative-execution and microarchitectural-buffer policy, then the
     * machine-check banks, then split-lock detection. All three record global
     * decisions that cpu_enable_features_ap() replays verbatim. */
    mitigations_init_bsp();
    mce_init();
    cpu_arm_split_lock_detect();

    kprintf("[CPU] Enabled:%s%s%s%s%s%s%s%s%s%s%s%s\n",
            g_pge_enabled       ? " PGE"      : "",
            g_umip_enabled      ? " UMIP"     : "",
            g_fsgsbase_enabled  ? " FSGSBASE" : "",
            g_smep_enabled      ? " SMEP"     : "",
            g_smap_enabled      ? " SMAP"     : "",
            g_osxsave_enabled   ? " OSXSAVE"  : "",
            g_pku_enabled       ? " PKU"      : "",
            g_pcid_enabled      ? " PCID"     : "",
            g_invpcid_enabled   ? " INVPCID"  : "",
            g_mwait_idle_enabled? " MWAIT"    : "",
            g_mce_enabled       ? " MCE"      : "",
            g_split_lock_detect ? " SPLITLOCK": "");
    kprintf("[CPU] CR4 pinned: 0x%llx (WP set, PCE denied to ring 3)\n",
            (unsigned long long)g_cr4_pinned);
}

/* ── Split-lock detection ────────────────────────────────────────────────── *
 * A locked access that straddles two cache lines forces the core to take the
 * bus lock, stalling every other core on the socket for microseconds. It needs
 * no privilege: a ring-3 loop of split LOCK instructions is a working denial of
 * service against the whole machine. Arming this turns each one into an #AC the
 * kernel can attribute to a process instead of an unexplained system-wide
 * stall. idt.c handles the fault; see cpu_split_lock_fault().
 * ------------------------------------------------------------------------- */
void cpu_arm_split_lock_detect(void)
{
    if (!g_cpu_info.has_split_lock_detect) return;
    u64 test_ctrl = rdmsr(MSR_IA32_TEST_CTRL);
    wrmsr(MSR_IA32_TEST_CTRL, test_ctrl | TEST_CTRL_SPLIT_LOCK_AC);
    g_split_lock_detect = 1;
}

void cpu_disarm_split_lock_detect(void)
{
    if (!g_cpu_info.has_split_lock_detect) return;
    wrmsr(MSR_IA32_TEST_CTRL, rdmsr(MSR_IA32_TEST_CTRL) & ~TEST_CTRL_SPLIT_LOCK_AC);
}

static u64 s_split_lock_faults = 0;

u64 cpu_split_lock_count(void) { return __atomic_load_n(&s_split_lock_faults, __ATOMIC_RELAXED); }

void cpu_split_lock_fault(u64 rip, bool from_user)
{
    u64 n = __atomic_add_fetch(&s_split_lock_faults, 1, __ATOMIC_RELAXED);

    /* #AC is a fault, so the offending instruction has not retired. Clearing
     * the enable on this core lets the retry complete; anything else would
     * either loop forever on the same instruction or kill a process for a
     * performance bug it may not even own. Detection stays armed on the other
     * cores, so a persistent offender keeps being reported from wherever it
     * next runs. */
    cpu_disarm_split_lock_detect();

    if (n <= 8) {
        kprintf("[CPU] split lock at RIP=0x%016llx (%s); bus-locking access, "
                "detection disarmed on this core\n",
                (unsigned long long)rip, from_user ? "user" : "kernel");
    } else if (n == 9) {
        kprintf("[CPU] split lock: further reports suppressed\n");
    }
}

void cpu_enable_features_ap(void)
{
    apply_cr0();

    /* One write brings this core to the BSP's exact CR4, pinned bits included —
     * compose_cr4() already carries OSXSAVE when the BSP enabled it. Raising
     * the register in a single step means the protections are never
     * transiently absent on a core that is already executing kernel code. */
    cpu_write_cr4(compose_cr4(read_cr4()));

    if (g_osxsave_enabled) {
        /* XCR0 must end up bit-identical to the BSP's, or a migrated thread's
         * XRSTOR faults the first time it runs here. */
        xsetbv(0, g_xcr0_mask);
    }

    if (g_pku_enabled) wrpkru(PKRU_INIT);
    if (g_cpu_info.has_xsaves) wrmsr(MSR_IA32_XSS, 0);

    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_NXE);

    if (g_cpu_info.has_rdtscp || g_cpu_info.has_rdpid) {
        /* ap_c_entry() programs GS_BASE before calling us, so the per-CPU block
         * is already reachable; read the id straight out of it rather than
         * pulling the whole SMP header into the arch feature code. */
        u32 cpu_id;
        __asm__ volatile("mov %%gs:0x10, %0" : "=r"(cpu_id));
        wrmsr(MSR_TSC_AUX, cpu_id);
    }


    /* Replay the BSP's policy decisions. Every one of these is per-logical-
     * processor state: a core that skipped them would run the same threads
     * with a different mitigation and a different fault behaviour. */
    mitigations_init_ap();
    mce_init_ap();
    if (g_split_lock_detect) cpu_arm_split_lock_detect();

    /* Per-core MSR state for the accelerated primitives (TPAUSE residency). */
    hwaccel_init_ap();

    /* Same story for the performance counters: PERFEVTSEL and PERF_GLOBAL_CTRL
     * are per-logical-processor, so a core that skipped this would count
     * nothing for a task that happened to be scheduled onto it. */
    extern void pmu_init_ap(void);
    pmu_init_ap();

    /* Confirm this core came up with the same protections the BSP pinned. */
    cpu_check_control_regs("AP bring-up");
}

/* ── Idle ────────────────────────────────────────────────────────────────── */

void cpu_idle_arm(const volatile void *monitor_addr)
{
    if (g_mwait_idle_enabled && monitor_addr)
        monitor_op((const void *)monitor_addr, 0, 0);
}

void cpu_idle_wait(void)
{
    if (g_mwait_idle_enabled) {
        /* STI's shadow covers exactly the next instruction, so an interrupt
         * arriving here cannot land between the unmask and the wait. */
        __asm__ volatile("sti; mwait" : : "a"(s_mwait_hint), "c"(0) : "memory");
        return;
    }
    __asm__ volatile("sti; hlt");
}

/* ── Linux-compatible flag names table for /proc/cpuinfo ─────────────────── */
struct cpu_flag_desc {
    u64 mask;
    const char *name;
};

static const struct cpu_flag_desc s_feat_edx[] = {
    { CPU_FEAT_FPU,     "fpu" },
    { CPU_FEAT_VME,     "vme" },
    { CPU_FEAT_DE,      "de" },
    { CPU_FEAT_PSE,     "pse" },
    { CPU_FEAT_TSC,     "tsc" },
    { CPU_FEAT_MSR,     "msr" },
    { CPU_FEAT_PAE,     "pae" },
    { CPU_FEAT_MCE,     "mce" },
    { CPU_FEAT_CX8,     "cx8" },
    { CPU_FEAT_APIC,    "apic" },
    { CPU_FEAT_SEP,     "sep" },
    { CPU_FEAT_MTRR,    "mtrr" },
    { CPU_FEAT_PGE,     "pge" },
    { CPU_FEAT_MCA,     "mca" },
    { CPU_FEAT_CMOV,    "cmov" },
    { CPU_FEAT_PAT,     "pat" },
    { CPU_FEAT_PSE36,   "pse36" },
    { CPU_FEAT_CLFSH,   "clflush" },
    { CPU_FEAT_MMX,     "mmx" },
    { CPU_FEAT_FXSR,    "fxsr" },
    { CPU_FEAT_SSE,     "sse" },
    { CPU_FEAT_SSE2,    "sse2" },
    { CPU_FEAT_HTT,     "ht" },
    { 0, NULL }
};

static const struct cpu_flag_desc s_feat_ecx[] = {
    { CPU_FEAT_SSE3,         "pni" },
    { CPU_FEAT_PCLMULQDQ,    "pclmulqdq" },
    { CPU_FEAT_DTES64,       "dtes64" },
    { CPU_FEAT_MONITOR,      "monitor" },
    { CPU_FEAT_SSSE3,        "ssse3" },
    { CPU_FEAT_FMA,          "fma" },
    { CPU_FEAT_CMPXCHG16B,   "cx16" },
    { CPU_FEAT_PCID,         "pcid" },
    { CPU_FEAT_SSE4_1,       "sse4_1" },
    { CPU_FEAT_SSE4_2,       "sse4_2" },
    { CPU_FEAT_X2APIC,       "x2apic" },
    { CPU_FEAT_MOVBE,        "movbe" },
    { CPU_FEAT_POPCNT,       "popcnt" },
    { CPU_FEAT_TSC_DEADLINE, "tsc_deadline_timer" },
    { CPU_FEAT_AES,          "aes" },
    { CPU_FEAT_XSAVE,        "xsave" },
    { CPU_FEAT_OSXSAVE,      "osxsave" },
    { CPU_FEAT_AVX,          "avx" },
    { CPU_FEAT_F16C,         "f16c" },
    { CPU_FEAT_RDRAND,       "rdrand" },
    { CPU_FEAT_HYPERVISOR,   "hypervisor" },
    { 0, NULL }
};

static const struct cpu_flag_desc s_feat_ext[] = {
    { CPU_EXT_FSGSBASE,      "fsgsbase" },
    { CPU_EXT_TSC_ADJUST,    "tsc_adjust" },
    { CPU_EXT_SGX,           "sgx" },
    { CPU_EXT_BMI1,          "bmi1" },
    { CPU_EXT_HLE,           "hle" },
    { CPU_EXT_AVX2,          "avx2" },
    { CPU_EXT_SMEP,          "smep" },
    { CPU_EXT_BMI2,          "bmi2" },
    { CPU_EXT_ERMS,          "erms" },
    { CPU_EXT_INVPCID,       "invpcid" },
    { CPU_EXT_RTM,           "rtm" },
    { CPU_EXT_CQM,           "cqm" },
    { CPU_EXT_MPX,           "mpx" },
    { CPU_EXT_RDT_A,         "rdt_a" },
    { CPU_EXT_AVX512F,       "avx512f" },
    { CPU_EXT_AVX512DQ,      "avx512dq" },
    { CPU_EXT_RDSEED,        "rdseed" },
    { CPU_EXT_ADX,           "adx" },
    { CPU_EXT_SMAP,          "smap" },
    { CPU_EXT_AVX512_IFMA,   "avx512ifma" },
    { CPU_EXT_CLFLUSHOPT,    "clflushopt" },
    { CPU_EXT_CLWB,          "clwb" },
    { CPU_EXT_INTEL_PT,      "intel_pt" },
    { CPU_EXT_AVX512PF,      "avx512pf" },
    { CPU_EXT_AVX512ER,      "avx512er" },
    { CPU_EXT_AVX512CD,      "avx512cd" },
    { CPU_EXT_SHA,           "sha_ni" },
    { CPU_EXT_AVX512BW,      "avx512bw" },
    { CPU_EXT_AVX512VL,      "avx512vl" },
    { CPU_EXT_PREFETCHWT1,   "prefetchwt1" },
    { CPU_EXT_AVX512_VBMI,   "avx512vbmi" },
    { CPU_EXT_UMIP,          "umip" },
    { CPU_EXT_PKU,           "pku" },
    { CPU_EXT_OSPKE,         "ospke" },
    { CPU_EXT_WAITPKG,       "waitpkg" },
    { CPU_EXT_AVX512_VBMI2,  "avx512_vbmi2" },
    { CPU_EXT_CET_SS,        "shstk" },
    { CPU_EXT_GFNI,          "gfni" },
    { CPU_EXT_VAES,          "vaes" },
    { CPU_EXT_VPCLMULQDQ,    "vpclmulqdq" },
    { CPU_EXT_AVX512_VNNI,   "avx512_vnni" },
    { CPU_EXT_AVX512_BITALG, "avx512_bitalg" },
    { CPU_EXT_AVX512_VPOPCNTDQ, "avx512_vpopcntdq" },
    { CPU_EXT_LA57,          "la57" },
    { CPU_EXT_RDPID,         "rdpid" },
    { CPU_EXT_KEYLOCKER,     "keylocker" },
    { CPU_EXT_BUS_LOCK_DETECT, "bus_lock_detect" },
    { CPU_EXT_CLDEMOTE,      "cldemote" },
    { CPU_EXT_MOVDIRI,       "movdiri" },
    { CPU_EXT_MOVDIR64B,     "movdir64b" },
    { CPU_EXT_ENQCMD,        "enqcmd" },
    { CPU_EXT_SGX_LC,        "sgx_lc" },
    { CPU_EXT_PKS,           "pks" },
    { 0, NULL }
};

static const struct cpu_flag_desc s_feat_extd[] = {
    { CPU_EXTD_AVX512_4VNNIW, "avx512_4vnniw" },
    { CPU_EXTD_AVX512_4FMAPS, "avx512_4fmaps" },
    { CPU_EXTD_FSRM,          "fsrm" },
    { CPU_EXTD_UINTR,         "uintr" },
    { CPU_EXTD_AVX512_VP2I,   "avx512_vp2intersect" },
    { CPU_EXTD_MD_CLEAR,      "md_clear" },
    { CPU_EXTD_RTM_ALWAYS_ABT,"rtm_always_abort" },
    { CPU_EXTD_TSX_FORCE_ABT, "tsx_force_abort" },
    { CPU_EXTD_SERIALIZE,     "serialize" },
    { CPU_EXTD_HYBRID,        "hybrid_cpu" },
    { CPU_EXTD_TSXLDTRK,      "tsxldtrk" },
    { CPU_EXTD_PCONFIG,       "pconfig" },
    { CPU_EXTD_ARCH_LBR,      "arch_lbr" },
    { CPU_EXTD_IBT,           "ibt" },
    { CPU_EXTD_AMX_BF16,      "amx_bf16" },
    { CPU_EXTD_AVX512_FP16,   "avx512_fp16" },
    { CPU_EXTD_AMX_TILE,      "amx_tile" },
    { CPU_EXTD_AMX_INT8,      "amx_int8" },
    { CPU_EXTD_IBRS,          "ibrs" },
    { CPU_EXTD_STIBP,         "stibp" },
    { CPU_EXTD_L1D_FLUSH,     "flush_l1d" },
    { CPU_EXTD_ARCH_CAPS,     "arch_capabilities" },
    { CPU_EXTD_CORE_CAPS,     "core_capabilities" },
    { CPU_EXTD_SSBD,          "ssbd" },
    { 0, NULL }
};

static const struct cpu_flag_desc s_feat_ext1[] = {
    { CPU_EXT1_SHA512,       "sha512" },
    { CPU_EXT1_SM3,          "sm3" },
    { CPU_EXT1_SM4,          "sm4" },
    { CPU_EXT1_AVX_VNNI,     "avx_vnni" },
    { CPU_EXT1_AVX512_BF16,  "avx512_bf16" },
    { CPU_EXT1_LASS,         "lass" },
    { CPU_EXT1_CMPCCXADD,    "cmpccxadd" },
    { CPU_EXT1_FZRM,         "fzrm" },
    { CPU_EXT1_FSRS,         "fsrs" },
    { CPU_EXT1_FSRC,         "fsrc" },
    { CPU_EXT1_FRED,         "fred" },
    { CPU_EXT1_LKGS,         "lkgs" },
    { CPU_EXT1_WRMSRNS,      "wrmsrns" },
    { CPU_EXT1_AMX_FP16,     "amx_fp16" },
    { CPU_EXT1_HRESET,       "hreset" },
    { CPU_EXT1_AVX_IFMA,     "avx_ifma" },
    { CPU_EXT1_LAM,          "lam" },
    { CPU_EXT1_MSRLIST,      "msrlist" },
    { 0, NULL }
};

/* Leaf 7 subleaf 2 EDX — the second-generation branch-prediction controls. */
static const struct cpu_flag_desc s_feat_ext2d[] = {
    { CPU_EXT2D_PSFD,          "psfd" },
    { CPU_EXT2D_IPRED_CTRL,    "ipred_ctrl" },
    { CPU_EXT2D_RRSBA_CTRL,    "rrsba_ctrl" },
    { CPU_EXT2D_DDPD_U,        "ddpd_u" },
    { CPU_EXT2D_BHI_CTRL,      "bhi_ctrl" },
    { CPU_EXT2D_MCDT_NO,       "mcdt_no" },
    { CPU_EXT2D_UC_LOCK_DIS,   "uc_lock_disable" },
    { CPU_EXT2D_MONITOR_MITG_NO, "monitor_mitg_no" },
    { 0, NULL }
};

/* Leaf 0x80000021 EAX — AMD's extended feature word 2. */
static const struct cpu_flag_desc s_feat_amd2[] = {
    { CPU_AMD2_NO_NESTED_DBP, "no_nested_data_bp" },
    { CPU_AMD2_FSGS_NS,       "fsgsbase_ns" },
    { CPU_AMD2_LFENCE_SERIAL, "lfence_rdtsc" },
    { CPU_AMD2_NULL_SEL_CLR,  "null_sel_clr_base" },
    { CPU_AMD2_AUTOIBRS,      "autoibrs" },
    { CPU_AMD2_SBPB,          "sbpb" },
    { CPU_AMD2_IBPB_BRTYPE,   "ibpb_brtype" },
    { CPU_AMD2_SRSO_NO,       "srso_no" },
    { 0, NULL }
};

static const struct cpu_flag_desc s_feat_ext2[] = {
    { CPU_EXT2_SYSCALL,      "syscall" },
    { CPU_EXT2_NX,           "nx" },
    { CPU_EXT2_1GB_PAGE,     "pdpe1gb" },
    { CPU_EXT2_RDTSCP,       "rdtscp" },
    { CPU_EXT2_LM,           "lm" },
    { CPU_EXT2_LAHF_LM,      "lahf_lm" },
    { CPU_EXT2_ABM,          "abm" },
    { CPU_EXT2_SSE4A,        "sse4a" },
    { CPU_EXT2_3DNOWPREFETCH,"3dnowprefetch" },
    { CPU_EXT2_TOPOEXT,      "topoext" },
    { CPU_EXT2_MWAITX,       "mwaitx" },
    { 0, NULL }
};

static const struct cpu_flag_desc s_feat_ext1d[] = {
    { CPU_EXT1D_AVX_VNNI_INT8,  "avx_vnni_int8" },
    { CPU_EXT1D_AVX_NE_CONVERT, "avx_ne_convert" },
    { CPU_EXT1D_AVX_VNNI_INT16, "avx_vnni_int16" },
    { CPU_EXT1D_PREFETCHI,      "prefetchi" },
    { CPU_EXT1D_AVX10,          "avx10" },
    { CPU_EXT1D_APX_F,          "apx_f" },
    { 0, NULL }
};

static const struct cpu_flag_desc s_feat_ext3[] = {
    { CPU_EXT3_CLZERO,         "clzero" },
    { CPU_EXT3_RETIRED_IP,     "irperf" },
    { CPU_EXT3_XSAVEERPTR,     "xsaveerptr" },
    { CPU_EXT3_RDPRU,          "rdpru" },
    { CPU_EXT3_MCOMMIT,        "mcommit" },
    { CPU_EXT3_WBNOINVD,       "wbnoinvd" },
    { CPU_EXT3_IBPB,           "ibpb" },
    { CPU_EXT3_IBRS,           "amd_ibrs" },
    { CPU_EXT3_STIBP,          "amd_stibp" },
    { CPU_EXT3_IBRS_ALWAYS,    "ibrs_enhanced" },
    { CPU_EXT3_STIBP_ALWAYS,   "amd_stibp_always_on" },
    { CPU_EXT3_IBRS_PREFERRED, "ibrs_preferred" },
    { CPU_EXT3_IBRS_SAME_MODE, "ibrs_same_mode" },
    { CPU_EXT3_AMD_PPIN,       "amd_ppin" },
    { CPU_EXT3_SSBD,           "amd_ssbd" },
    { CPU_EXT3_SSB_NO,         "ssb_no" },
    { CPU_EXT3_CPPC,           "cppc" },
    { CPU_EXT3_AMD_PSFD,       "amd_psfd" },
    { 0, NULL }
};

static const struct cpu_flag_desc s_feat_pm[] = {
    { CPU_PM_INVARIANT_TSC,  "constant_tsc" },
    { 0, NULL }
};

/* Append every set flag from @list to @buf, space-separated. Uses scnprintf so
 * a full buffer truncates instead of running the accumulator past its end. */
static size_t append_flags(char *buf, size_t max, size_t off,
                           const struct cpu_flag_desc *list, u64 val)
{
    for (int i = 0; list[i].name; i++) {
        if (val & list[i].mask) {
            off += scnprintf(buf + off, max > off ? max - off : 0, "%s%s",
                             off > 0 ? " " : "", list[i].name);
        }
    }
    return off;
}

size_t cpu_format_flags(char *buf, size_t max)
{
    size_t off = 0;
    off = append_flags(buf, max, off, s_feat_edx,  g_cpu_info.features);
    off = append_flags(buf, max, off, s_feat_ecx,  g_cpu_info.features);
    off = append_flags(buf, max, off, s_feat_ext,  g_cpu_info.ext_features);
    off = append_flags(buf, max, off, s_feat_extd, g_cpu_info.extd_features);
    off = append_flags(buf, max, off, s_feat_ext1, g_cpu_info.ext1_features);
    off = append_flags(buf, max, off, s_feat_ext2d, g_cpu_info.ext2d_features);
    off = append_flags(buf, max, off, s_feat_amd2, g_cpu_info.amd2_features);
    off = append_flags(buf, max, off, s_feat_ext1d, g_cpu_info.ext1d_features);
    off = append_flags(buf, max, off, s_feat_ext2, g_cpu_info.ext2_features);
    off = append_flags(buf, max, off, s_feat_ext3, g_cpu_info.ext3_features);
    off = append_flags(buf, max, off, s_feat_pm,   g_cpu_info.pm_features);
    return off;
}

size_t cpu_format_bugs(char *buf, size_t max)
{
    size_t off = 0;
    /* A CPU that does not enumerate IA32_ARCH_CAPABILITIES predates the "no"
     * bits entirely, so absence of the MSR means "assume vulnerable" — the same
     * reading Linux takes. This list names errata the *silicon* has, whether or
     * not the kernel mitigated them; what was done about each is reported
     * separately by mitigations_format(). */
    u64 caps = g_cpu_info.arch_caps;
    bool intel = cpu_is_intel();
    bool amd   = cpu_is_amd();

    #define BUG(cond, name) do { \
        if (cond) off += scnprintf(buf + off, max > off ? max - off : 0, \
                                   "%s%s", off > 0 ? " " : "", name); \
    } while (0)

    BUG(intel && !(caps & ARCH_CAP_RDCL_NO), "meltdown");
    BUG(1,                                   "spectre_v1");
    BUG(!(caps & ARCH_CAP_IBRS_ALL),         "spectre_v2");
    BUG(!(caps & ARCH_CAP_SSB_NO) && !(g_cpu_info.ext3_features & CPU_EXT3_SSB_NO),
        "spec_store_bypass");
    BUG(intel && !(caps & ARCH_CAP_MDS_NO),  "mds");
    BUG(intel && (g_cpu_info.ext_features & CPU_EXT_RTM) && !(caps & ARCH_CAP_TAA_NO),
        "tsx_async_abort");

    /* L1TF and the page-size-change erratum are both Intel-only and both are
     * implied by the same "we can read stale L1 data" property RDCL_NO denies. */
    BUG(intel && !(caps & ARCH_CAP_RDCL_NO), "l1tf");
    BUG(intel && !(caps & ARCH_CAP_PSCHANGE_MC_NO), "itlb_multihit");

    /* The MMIO stale-data family: three separate "no" bits, any one of which
     * missing leaves a buffer an attacker can sample. */
    BUG(intel && !(caps & ARCH_CAP_SBDR_SSDP_NO) && !(caps & ARCH_CAP_FBSDP_NO)
              && !(caps & ARCH_CAP_PSDP_NO), "mmio_stale_data");

    BUG(intel && !(caps & ARCH_CAP_RFDS_NO) && (caps & ARCH_CAP_RFDS_CLEAR),
        "reg_file_data_sampling");
    BUG(intel && !(caps & ARCH_CAP_GDS_NO) && (caps & ARCH_CAP_GDS_CTRL),
        "gather_data_sampling");
    BUG(!(caps & ARCH_CAP_BHI_NO) && (g_cpu_info.ext2d_features & CPU_EXT2D_BHI_CTRL),
        "spectre_bhi");
    BUG(intel && (caps & ARCH_CAP_IBRS_ALL) && !(caps & ARCH_CAP_PBRSB_NO),
        "eibrs_pbrsb");

    /* Retbleed reaches the return predictor: Intel through the same path eIBRS
     * closes, AMD through its own, which SRSO_NO denies on fixed parts. */
    BUG(intel ? !(caps & ARCH_CAP_IBRS_ALL)
              : (amd && !(g_cpu_info.amd2_features & CPU_AMD2_SRSO_NO)),
        "retbleed");
    BUG(amd && !(g_cpu_info.amd2_features & CPU_AMD2_SRSO_NO), "spec_rstack_overflow");

    /* A split-lock-capable part that we did not arm is a live local DoS. */
    BUG(g_cpu_info.has_split_lock_detect && !g_split_lock_detect, "split_lock");
    #undef BUG
    return off;
}
