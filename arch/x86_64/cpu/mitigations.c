/* ============================================================================
 * AzamiOS — Speculative-Execution & Side-Channel Mitigation Policy (x86_64)
 * File: arch/x86_64/cpu/mitigations.c
 *
 * See mitigations.h for the two rules this file lives by. The shape of every
 * decision below is the same: ask IA32_ARCH_CAPABILITIES whether the part is
 * affected at all, then apply the cheapest control that actually closes the
 * hole, then record what happened so /proc can report it truthfully.
 * ============================================================================ */

#include "mitigations.h"
#include "cpu.h"
#include "msr.h"
#include "gdt.h"
#include "../../../kernel/lib/string.h"

extern void kprintf(const char *fmt, ...);
extern int  scnprintf(char *buf, size_t size, const char *fmt, ...);

/* ── Policy, decided once on the BSP and replayed by every AP ─────────────── */
u8  g_verw_user_clear = 0;
u16 g_verw_sel        = SEL_KERNEL_DATA;
u8  g_spectre_v2_mode = SPECTRE_V2_NONE;
u8  g_ibpb_on_switch  = 0;
u8  g_ssbd_enabled    = 0;
u8  g_tsx_disabled    = 0;

/* The exact IA32_SPEC_CTRL value every core must carry. Computed on the BSP so
 * an AP cannot derive a different one from a slightly different CPUID view
 * (which happens on hybrid parts, where the E-cores enumerate less). */
static u64 s_spec_ctrl = 0;
static u8  s_spec_ctrl_valid = 0;
static u8  s_autoibrs = 0;

/* Number of IBPBs issued, so the cost of the switch-time barrier is visible
 * rather than a mystery in a profile. */
static u64 s_ibpb_count = 0;

/* ── Individual controls ─────────────────────────────────────────────────── */

/* Spectre-v2: branch target injection.
 *
 * Enhanced IBRS (Intel, IBRS_ALL) and AutoIBRS (AMD) both mean "the hardware
 * keeps indirect branch predictions isolated across privilege levels once you
 * ask for it". Both are set-once. Legacy IBRS — the version needing an MSR
 * write on every kernel entry and exit — is deliberately not implemented: it
 * costs more than this kernel's whole syscall path on the parts that need it,
 * and IBPB-on-switch covers the cross-process case that actually matters here.
 */
static void configure_spectre_v2(void)
{
    if (cpu_is_amd() && (g_cpu_info.amd2_features & CPU_AMD2_AUTOIBRS)) {
        s_autoibrs = 1;
        g_spectre_v2_mode = SPECTRE_V2_AUTOIBRS;
        return;
    }

    if ((g_cpu_info.extd_features & CPU_EXTD_IBRS) &&
        (g_cpu_info.arch_caps & ARCH_CAP_IBRS_ALL)) {
        s_spec_ctrl |= SPEC_CTRL_IBRS;
        s_spec_ctrl_valid = 1;
        g_spectre_v2_mode = SPECTRE_V2_EIBRS;

        /* On an eIBRS part that still has restricted-RSB behaviour, indirect
         * predictions can fall back to the BTB; RRSBA_DIS_S closes that for
         * ring 0 without any per-transition cost. Same for BHI_DIS_S, which
         * fences the branch-history side of the same attack. */
        if ((g_cpu_info.arch_caps & ARCH_CAP_RRSBA) &&
            (g_cpu_info.ext2d_features & CPU_EXT2D_RRSBA_CTRL))
            s_spec_ctrl |= SPEC_CTRL_RRSBA_DIS_S;
        if (!(g_cpu_info.arch_caps & ARCH_CAP_BHI_NO) &&
            (g_cpu_info.ext2d_features & CPU_EXT2D_BHI_CTRL))
            s_spec_ctrl |= SPEC_CTRL_BHI_DIS_S;
        return;
    }

    /* No always-on isolation. IBPB at each address-space change is what is
     * left: it does not protect the kernel from a user-trained predictor
     * within one process, but it does stop one process steering another's
     * indirect branches, which is the boundary a multi-user system cares
     * about most. */
    if (g_cpu_info.has_ibpb) {
        g_ibpb_on_switch  = 1;
        g_spectre_v2_mode = SPECTRE_V2_IBPB;
    }
}

/* Cross-thread branch prediction (Spectre-v2 user-to-user over SMT).
 * Only meaningful when the package actually has sibling threads sharing a
 * predictor — on a part without SMT this is a wasted MSR bit. */
static void configure_stibp(void)
{
    if (!(g_cpu_info.extd_features & CPU_EXTD_STIBP)) return;
    if (g_cpu_info.threads_per_core <= 1) return;
    /* AutoIBRS already implies cross-thread isolation on the parts that have
     * it, and STIBP is redundant under enhanced IBRS on Intel. */
    if (s_autoibrs) return;

    s_spec_ctrl |= SPEC_CTRL_STIBP;
    s_spec_ctrl_valid = 1;
}

/* Spectre-v4: speculative store bypass. SSBD is a real throughput cost on
 * store-heavy code, so it is only set where the part says it is affected. */
static void configure_ssbd(void)
{
    if (g_cpu_info.arch_caps & ARCH_CAP_SSB_NO) return;
    if (g_cpu_info.ext3_features & CPU_EXT3_SSB_NO) return;
    if (!g_cpu_info.has_ssbd) return;

    s_spec_ctrl |= SPEC_CTRL_SSBD;
    s_spec_ctrl_valid = 1;
    g_ssbd_enabled = 1;
}

/* Predictive store forwarding: same shape as SSBD, a different predictor, and
 * two independent enumerations for the same SPEC_CTRL bit — AMD advertises it
 * in leaf 0x80000008 EBX, Intel in leaf 7 subleaf 2 EDX. Either is sufficient;
 * requiring both would silently skip the mitigation on every part that has it. */
static void configure_psfd(void)
{
    bool available = (g_cpu_info.ext2d_features & CPU_EXT2D_PSFD) ||
                     (g_cpu_info.ext3_features  & CPU_EXT3_AMD_PSFD);
    if (!available) return;

    s_spec_ctrl |= SPEC_CTRL_PSFD;
    s_spec_ctrl_valid = 1;
}

/* MDS, TAA, MMIO stale data and RFDS are four names for the same shape of
 * leak: stale data left in a microarchitectural buffer that a later faulting
 * or assisting load can sample. Microcode gives them one shared fix — VERW,
 * repurposed to overwrite those buffers — which the kernel must execute on
 * every path back to ring 3. The entry stubs read g_verw_user_clear directly.
 */
static void configure_buffer_clear(void)
{
    bool affected = false;

    if (cpu_is_intel()) {
        if (!(g_cpu_info.arch_caps & ARCH_CAP_MDS_NO)) affected = true;

        /* TAA reaches the same buffers through an aborting transaction, so a
         * part with usable TSX and no TAA_NO needs the clear even if MDS_NO
         * says the original vector is fixed. */
        if ((g_cpu_info.ext_features & CPU_EXT_RTM) &&
            !(g_cpu_info.arch_caps & ARCH_CAP_TAA_NO) && !g_tsx_disabled)
            affected = true;

        /* FB_CLEAR is set on parts where VERW was *extended* to the fill
         * buffers — its presence means there is something there to clear. */
        if (g_cpu_info.arch_caps & ARCH_CAP_FB_CLEAR) affected = true;

        /* Register file data sampling: same instruction, newer buffer. */
        if ((g_cpu_info.arch_caps & ARCH_CAP_RFDS_CLEAR) &&
            !(g_cpu_info.arch_caps & ARCH_CAP_RFDS_NO))
            affected = true;
    }

    if (!affected) return;

    if (!g_cpu_info.has_md_clear && !(g_cpu_info.arch_caps & ARCH_CAP_FB_CLEAR)) {
        /* Affected, but the microcode that gives VERW its clearing behaviour
         * is not loaded. Running VERW anyway would do nothing but cost cycles
         * and would let /proc claim a mitigation that is not there. */
        return;
    }

    g_verw_sel = SEL_KERNEL_DATA;
    g_verw_user_clear = 1;
}

/* TSX is the delivery mechanism for TAA and has no user in this kernel. Where
 * microcode offers IA32_TSX_CTRL, switching it off removes the whole class
 * outright — strictly better than clearing buffers on every return from a
 * transaction that should not have been possible in the first place. */
static void configure_tsx(void)
{
    if (!cpu_is_intel()) return;
    if (!(g_cpu_info.ext_features & (CPU_EXT_RTM | CPU_EXT_HLE))) return;
    if (!(g_cpu_info.arch_caps & ARCH_CAP_TSX_CTRL)) return;
    if (g_cpu_info.arch_caps & ARCH_CAP_TAA_NO) return;   /* not affected */

    u64 v = rdmsr(MSR_IA32_TSX_CTRL);
    v |= TSX_CTRL_RTM_DISABLE | TSX_CTRL_CPUID_CLEAR;
    wrmsr(MSR_IA32_TSX_CTRL, v);
    g_tsx_disabled = 1;
}

/* AMD parts in the 15h–17h range need LFENCE to be dispatch-serialising for
 * the Spectre-v1 barrier below to mean anything; the bit is architectural on
 * later families and enumerated by leaf 0x80000021. */
static void configure_lfence_serialize(void)
{
    if (!cpu_is_amd()) return;
    if (g_cpu_info.amd2_features & CPU_AMD2_LFENCE_SERIAL) return;  /* already */
    if (g_cpu_info.family < 0x10 || g_cpu_info.family > 0x17) return;

    u64 v = rdmsr(MSR_AMD64_DE_CFG);
    if (!(v & DE_CFG_LFENCE_SERIALIZE))
        wrmsr(MSR_AMD64_DE_CFG, v | DE_CFG_LFENCE_SERIALIZE);
}

/* ── Applying the decided policy to one core ─────────────────────────────── */

static void apply_to_this_cpu(void)
{
    if (s_spec_ctrl_valid) {
        /* Preserve any bits firmware set that we have no opinion about. */
        u64 cur = rdmsr(MSR_IA32_SPEC_CTRL);
        wrmsr(MSR_IA32_SPEC_CTRL, cur | s_spec_ctrl);
    }
    if (s_autoibrs)
        wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_AUTOIBRS);
    if (g_tsx_disabled)
        wrmsr(MSR_IA32_TSX_CTRL,
              rdmsr(MSR_IA32_TSX_CTRL) | TSX_CTRL_RTM_DISABLE | TSX_CTRL_CPUID_CLEAR);
}

void mitigations_init_bsp(void)
{
    configure_tsx();              /* before the buffer-clear decision reads it */
    configure_spectre_v2();
    configure_stibp();
    configure_ssbd();
    configure_psfd();
    configure_buffer_clear();
    configure_lfence_serialize();

    apply_to_this_cpu();

    char line[192];
    mitigations_format_short(line, sizeof line);
    kprintf("[CPU] Mitigations: %s\n", line);
}

void mitigations_init_ap(void)
{
    apply_to_this_cpu();
    configure_lfence_serialize();
}

void mitigations_switch_mm(u64 prev_pml4, u64 next_pml4)
{
    if (!g_ibpb_on_switch) return;
    if (prev_pml4 == next_pml4) return;   /* same address space: nothing gained */

    wrmsr(MSR_IA32_PRED_CMD, PRED_CMD_IBPB);
    __atomic_add_fetch(&s_ibpb_count, 1, __ATOMIC_RELAXED);
}

void mitigations_flush_l1d(void)
{
    if (!g_cpu_info.has_l1d_flush) return;
    wrmsr(MSR_IA32_FLUSH_CMD, FLUSH_CMD_L1D);
}

/* ── Reporting ───────────────────────────────────────────────────────────── */

static const char *spectre_v2_status(void)
{
    switch (g_spectre_v2_mode) {
    case SPECTRE_V2_EIBRS:    return "Mitigation: Enhanced IBRS";
    case SPECTRE_V2_AUTOIBRS: return "Mitigation: Enhanced IBRS (AutoIBRS)";
    case SPECTRE_V2_IBPB:     return "Mitigation: IBPB on address-space switch";
    default:                  return "Vulnerable";
    }
}

size_t mitigations_format(char *buf, size_t max)
{
    u64  caps  = g_cpu_info.arch_caps;
    bool intel = cpu_is_intel();
    size_t off = 0;

    #define LINE(name, text) \
        off += scnprintf(buf + off, max > off ? max - off : 0, \
                         "%-22s %s\n", name ":", text)

    /* Meltdown. The fix is a split page table (KPTI), which this kernel does
     * not implement — so an affected part is reported as affected. */
    LINE("meltdown", (!intel || (caps & ARCH_CAP_RDCL_NO))
                     ? "Not affected" : "Vulnerable: no page-table isolation");

    /* Spectre-v1. The syscall dispatcher masks the table index with
     * array_index_nospec() so a mispredicted bounds check cannot speculatively
     * load past the table; see nospec.h for the technique. */
    LINE("spectre_v1", "Mitigation: array_index_nospec on syscall dispatch");

    LINE("spectre_v2", spectre_v2_status());

    LINE("spec_store_bypass",
         (caps & ARCH_CAP_SSB_NO) || (g_cpu_info.ext3_features & CPU_EXT3_SSB_NO)
            ? "Not affected"
            : g_ssbd_enabled ? "Mitigation: Speculative Store Bypass disabled"
                             : "Vulnerable");

    LINE("mds", (intel && !(caps & ARCH_CAP_MDS_NO))
                ? (g_verw_user_clear ? "Mitigation: Clear CPU buffers"
                                     : "Vulnerable: no microcode")
                : "Not affected");

    LINE("tsx_async_abort",
         !intel || (caps & ARCH_CAP_TAA_NO) ? "Not affected"
         : g_tsx_disabled                   ? "Mitigation: TSX disabled"
         : !(g_cpu_info.ext_features & CPU_EXT_RTM) ? "Not affected"
         : g_verw_user_clear                ? "Mitigation: Clear CPU buffers"
                                            : "Vulnerable");

    LINE("mmio_stale_data",
         !intel || (caps & (ARCH_CAP_FBSDP_NO | ARCH_CAP_PSDP_NO | ARCH_CAP_SBDR_SSDP_NO))
                                            ? "Not affected"
         : g_verw_user_clear                ? "Mitigation: Clear CPU buffers"
                                            : "Vulnerable");

    LINE("reg_file_data_sampling",
         !intel || (caps & ARCH_CAP_RFDS_NO) ? "Not affected"
         : !(caps & ARCH_CAP_RFDS_CLEAR)     ? "Vulnerable: no microcode"
         : g_verw_user_clear                 ? "Mitigation: Clear CPU buffers"
                                             : "Vulnerable");

    LINE("gather_data_sampling",
         !intel || (caps & ARCH_CAP_GDS_NO) ? "Not affected"
                                            : "Vulnerable: microcode dependent");

    LINE("srbds", intel ? "Unknown: microcode dependent" : "Not affected");

    /* L1TF is fixed by inverting the PFN of non-present PTEs so a speculative
     * walk lands outside physical memory. That is a VMM change, not an MSR. */
    LINE("l1tf", (!intel || (caps & ARCH_CAP_RDCL_NO))
                 ? "Not affected" : "Vulnerable: PTE inversion not implemented");

    LINE("retbleed",
         cpu_is_amd() ? ((g_cpu_info.amd2_features & CPU_AMD2_SRSO_NO)
                            ? "Not affected"
                            : (g_ibpb_on_switch ? "Mitigation: IBPB" : "Vulnerable"))
                      : (g_spectre_v2_mode == SPECTRE_V2_EIBRS
                            ? "Mitigation: Enhanced IBRS" : "Vulnerable"));

    LINE("spectre_bhb", (caps & ARCH_CAP_BHI_NO) ? "Not affected"
                        : (s_spec_ctrl & SPEC_CTRL_BHI_DIS_S)
                            ? "Mitigation: BHI_DIS_S" : "Vulnerable");

    LINE("split_lock", g_split_lock_detect ? "Detection: #AC on split lock"
                       : g_cpu_info.has_split_lock_detect ? "Available, disarmed"
                                                          : "Not supported");
    #undef LINE
    return off;
}

size_t mitigations_format_short(char *buf, size_t max)
{
    return (size_t)scnprintf(buf, max,
        "spectre_v2=%s ssbd=%s verw_user_clear=%s tsx=%s ibpb_switch=%llu",
        g_spectre_v2_mode == SPECTRE_V2_EIBRS    ? "eibrs"    :
        g_spectre_v2_mode == SPECTRE_V2_AUTOIBRS ? "autoibrs" :
        g_spectre_v2_mode == SPECTRE_V2_IBPB     ? "ibpb"     : "none",
        g_ssbd_enabled     ? "on" : "off",
        g_verw_user_clear  ? "on" : "off",
        g_tsx_disabled     ? "disabled" : "as-found",
        (unsigned long long)__atomic_load_n(&s_ibpb_count, __ATOMIC_RELAXED));
}
