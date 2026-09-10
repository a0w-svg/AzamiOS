/* ============================================================================
 * AzamiOS — CPU Feature Detection & Extensions Management (x86_64)
 * File: arch/x86_64/cpu/cpu.h
 * ============================================================================ */
#pragma once

#include "../../../include/azami/types.h"

/* ── Standard Feature Flags (CPUID Leaf 1 EDX) ────────────────────────────── */
#define CPU_FEAT_FPU            (1ULL << 0)
#define CPU_FEAT_VME            (1ULL << 1)
#define CPU_FEAT_DE             (1ULL << 2)
#define CPU_FEAT_PSE            (1ULL << 3)
#define CPU_FEAT_TSC            (1ULL << 4)
#define CPU_FEAT_MSR            (1ULL << 5)
#define CPU_FEAT_PAE            (1ULL << 6)
#define CPU_FEAT_MCE            (1ULL << 7)
#define CPU_FEAT_CX8            (1ULL << 8)
#define CPU_FEAT_APIC           (1ULL << 9)
#define CPU_FEAT_SEP            (1ULL << 11)
#define CPU_FEAT_MTRR           (1ULL << 12)
#define CPU_FEAT_PGE            (1ULL << 13)
#define CPU_FEAT_MCA            (1ULL << 14)
#define CPU_FEAT_CMOV           (1ULL << 15)
#define CPU_FEAT_PAT            (1ULL << 16)
#define CPU_FEAT_PSE36          (1ULL << 17)
#define CPU_FEAT_CLFSH          (1ULL << 19)
#define CPU_FEAT_MMX            (1ULL << 23)
#define CPU_FEAT_FXSR           (1ULL << 24)
#define CPU_FEAT_SSE            (1ULL << 25)
#define CPU_FEAT_SSE2           (1ULL << 26)
#define CPU_FEAT_HTT            (1ULL << 28)

/* ── Standard Feature Flags (CPUID Leaf 1 ECX) ────────────────────────────── */
#define CPU_FEAT_SSE3           (1ULL << 32)
#define CPU_FEAT_PCLMULQDQ      (1ULL << 33)
#define CPU_FEAT_DTES64         (1ULL << 34)
#define CPU_FEAT_MONITOR        (1ULL << 35)
#define CPU_FEAT_SSSE3          (1ULL << 41)
#define CPU_FEAT_FMA            (1ULL << 44)
#define CPU_FEAT_CMPXCHG16B     (1ULL << 45)
#define CPU_FEAT_PCID           (1ULL << 49)
#define CPU_FEAT_SSE4_1         (1ULL << 51)
#define CPU_FEAT_SSE4_2         (1ULL << 52)
#define CPU_FEAT_X2APIC         (1ULL << 53)
#define CPU_FEAT_MOVBE          (1ULL << 54)
#define CPU_FEAT_POPCNT         (1ULL << 55)
#define CPU_FEAT_TSC_DEADLINE   (1ULL << 56)
#define CPU_FEAT_AES            (1ULL << 57)
#define CPU_FEAT_XSAVE          (1ULL << 58)
#define CPU_FEAT_OSXSAVE        (1ULL << 59)
#define CPU_FEAT_AVX            (1ULL << 60)
#define CPU_FEAT_F16C           (1ULL << 61)
#define CPU_FEAT_RDRAND         (1ULL << 62)
#define CPU_FEAT_HYPERVISOR     (1ULL << 63)

/* ── Extended Feature Flags (CPUID Leaf 7 Subleaf 0 EBX) ──────────────────── */
#define CPU_EXT_FSGSBASE        (1ULL << 0)
#define CPU_EXT_TSC_ADJUST      (1ULL << 1)
#define CPU_EXT_SGX             (1ULL << 2)
#define CPU_EXT_BMI1            (1ULL << 3)
#define CPU_EXT_HLE             (1ULL << 4)
#define CPU_EXT_AVX2            (1ULL << 5)
#define CPU_EXT_SMEP            (1ULL << 7)
#define CPU_EXT_BMI2            (1ULL << 8)
#define CPU_EXT_ERMS            (1ULL << 9)
#define CPU_EXT_INVPCID         (1ULL << 10)
#define CPU_EXT_RTM             (1ULL << 11)
#define CPU_EXT_CQM             (1ULL << 12)  /* RDT monitoring              */
#define CPU_EXT_MPX             (1ULL << 14)
#define CPU_EXT_RDT_A           (1ULL << 15)  /* RDT allocation              */
#define CPU_EXT_AVX512F         (1ULL << 16)
#define CPU_EXT_AVX512DQ        (1ULL << 17)
#define CPU_EXT_RDSEED          (1ULL << 18)
#define CPU_EXT_ADX             (1ULL << 19)
#define CPU_EXT_SMAP            (1ULL << 20)
#define CPU_EXT_AVX512_IFMA     (1ULL << 21)
#define CPU_EXT_CLFLUSHOPT      (1ULL << 23)
#define CPU_EXT_CLWB            (1ULL << 24)
#define CPU_EXT_INTEL_PT        (1ULL << 25)  /* processor trace             */
#define CPU_EXT_AVX512PF        (1ULL << 26)
#define CPU_EXT_AVX512ER        (1ULL << 27)
#define CPU_EXT_AVX512CD        (1ULL << 28)
#define CPU_EXT_SHA             (1ULL << 29)
#define CPU_EXT_AVX512BW        (1ULL << 30)
#define CPU_EXT_AVX512VL        (1ULL << 31)

/* ── Extended Feature Flags (CPUID Leaf 7 Subleaf 0 ECX) ──────────────────── */
#define CPU_EXT_PREFETCHWT1     (1ULL << 32)
#define CPU_EXT_AVX512_VBMI     (1ULL << 33)
#define CPU_EXT_UMIP            (1ULL << 34)
#define CPU_EXT_PKU             (1ULL << 35)
#define CPU_EXT_OSPKE           (1ULL << 36)
#define CPU_EXT_WAITPKG         (1ULL << 37)  /* UMONITOR/UMWAIT/TPAUSE      */
#define CPU_EXT_AVX512_VBMI2    (1ULL << 38)
#define CPU_EXT_CET_SS          (1ULL << 39)  /* CET shadow stack            */
#define CPU_EXT_GFNI            (1ULL << 40)
#define CPU_EXT_VAES            (1ULL << 41)
#define CPU_EXT_VPCLMULQDQ      (1ULL << 42)
#define CPU_EXT_AVX512_VNNI     (1ULL << 43)
#define CPU_EXT_AVX512_BITALG   (1ULL << 44)
#define CPU_EXT_AVX512_VPOPCNTDQ (1ULL << 46)
#define CPU_EXT_LA57            (1ULL << 48)
#define CPU_EXT_RDPID           (1ULL << 54)
#define CPU_EXT_KEYLOCKER       (1ULL << 55)  /* AES Key Locker (ENCODEKEY)  */
#define CPU_EXT_BUS_LOCK_DETECT (1ULL << 56)  /* #DB on a bus lock           */
#define CPU_EXT_CLDEMOTE        (1ULL << 57)
#define CPU_EXT_MOVDIRI         (1ULL << 59)
#define CPU_EXT_MOVDIR64B       (1ULL << 60)
#define CPU_EXT_ENQCMD          (1ULL << 61)
#define CPU_EXT_SGX_LC          (1ULL << 62)
#define CPU_EXT_PKS             (1ULL << 63)  /* protection keys, supervisor */

/* ── Extended Feature Flags (CPUID Leaf 7 Subleaf 0 EDX) ──────────────────── */
#define CPU_EXTD_AVX512_4VNNIW  (1ULL << 2)
#define CPU_EXTD_AVX512_4FMAPS  (1ULL << 3)
#define CPU_EXTD_FSRM           (1ULL << 4)   /* fast short REP MOVSB */
#define CPU_EXTD_UINTR          (1ULL << 5)   /* user interrupts             */
#define CPU_EXTD_AVX512_VP2I    (1ULL << 8)
#define CPU_EXTD_MD_CLEAR       (1ULL << 10)  /* VERW flushes CPU buffers    */
#define CPU_EXTD_RTM_ALWAYS_ABT (1ULL << 11)  /* microcode killed RTM        */
#define CPU_EXTD_TSX_FORCE_ABT  (1ULL << 13)
#define CPU_EXTD_SERIALIZE      (1ULL << 14)
#define CPU_EXTD_HYBRID         (1ULL << 15)  /* P-core / E-core package     */
#define CPU_EXTD_TSXLDTRK       (1ULL << 16)
#define CPU_EXTD_PCONFIG        (1ULL << 18)
#define CPU_EXTD_ARCH_LBR       (1ULL << 19)  /* architectural last-branch   */
#define CPU_EXTD_IBT            (1ULL << 20)  /* CET indirect branch tracking */
#define CPU_EXTD_AMX_BF16       (1ULL << 22)
#define CPU_EXTD_AVX512_FP16    (1ULL << 23)
#define CPU_EXTD_AMX_TILE       (1ULL << 24)
#define CPU_EXTD_AMX_INT8       (1ULL << 25)
#define CPU_EXTD_IBRS           (1ULL << 26)  /* IBRS/IBPB via SPEC_CTRL */
#define CPU_EXTD_STIBP          (1ULL << 27)
#define CPU_EXTD_L1D_FLUSH      (1ULL << 28)
#define CPU_EXTD_ARCH_CAPS      (1ULL << 29)  /* IA32_ARCH_CAPABILITIES MSR */
#define CPU_EXTD_CORE_CAPS      (1ULL << 30)  /* IA32_CORE_CAPABILITIES MSR */
#define CPU_EXTD_SSBD           (1ULL << 31)

/* ── Extended Feature Flags (CPUID Leaf 7 Subleaf 1 EAX) ──────────────────── */
#define CPU_EXT1_SHA512         (1ULL << 0)
#define CPU_EXT1_SM3            (1ULL << 1)
#define CPU_EXT1_SM4            (1ULL << 2)
#define CPU_EXT1_AVX_VNNI       (1ULL << 4)
#define CPU_EXT1_AVX512_BF16    (1ULL << 5)
#define CPU_EXT1_LASS           (1ULL << 6)   /* linear address space sep.   */
#define CPU_EXT1_CMPCCXADD      (1ULL << 7)
#define CPU_EXT1_FZRM           (1ULL << 10)  /* fast zero-length REP MOVSB  */
#define CPU_EXT1_FSRS           (1ULL << 11)  /* fast short REP STOSB        */
#define CPU_EXT1_FSRC           (1ULL << 12)  /* fast short REP CMPSB/SCASB  */
#define CPU_EXT1_FRED           (1ULL << 17)  /* flexible return/event deliv.*/
#define CPU_EXT1_LKGS           (1ULL << 18)
#define CPU_EXT1_WRMSRNS        (1ULL << 19)
#define CPU_EXT1_AMX_FP16       (1ULL << 21)
#define CPU_EXT1_HRESET         (1ULL << 22)  /* history reset               */
#define CPU_EXT1_AVX_IFMA       (1ULL << 23)
#define CPU_EXT1_LAM            (1ULL << 26)  /* linear address masking      */
#define CPU_EXT1_MSRLIST        (1ULL << 27)

/* ── Speculation Controls (CPUID Leaf 7 Subleaf 2 EDX) ────────────────────── *
 * The second generation of branch-prediction controls: where leaf 7.0 said
 * "IBRS exists", these say which *specific* predictor can be fenced off, which
 * is what lets the mitigation engine turn on only what a given part needs.
 * ------------------------------------------------------------------------- */
#define CPU_EXT2D_PSFD          (1ULL << 0)   /* predictive store forwarding */
#define CPU_EXT2D_IPRED_CTRL    (1ULL << 1)   /* IPRED_DIS_U/S in SPEC_CTRL  */
#define CPU_EXT2D_RRSBA_CTRL    (1ULL << 2)   /* RRSBA_DIS_U/S in SPEC_CTRL  */
#define CPU_EXT2D_DDPD_U        (1ULL << 3)   /* data-dependent prefetch off */
#define CPU_EXT2D_BHI_CTRL      (1ULL << 4)   /* BHI_DIS_S in SPEC_CTRL      */
#define CPU_EXT2D_MCDT_NO       (1ULL << 5)   /* no MCDT behaviour           */
#define CPU_EXT2D_UC_LOCK_DIS   (1ULL << 6)
#define CPU_EXT2D_MONITOR_MITG_NO (1ULL << 7)

/* ── AMD Extended Features 2 (CPUID Leaf 0x80000021 EAX) ──────────────────── */
#define CPU_AMD2_NO_NESTED_DBP  (1ULL << 0)
#define CPU_AMD2_FSGS_NS        (1ULL << 1)   /* non-serialising base writes */
#define CPU_AMD2_LFENCE_SERIAL  (1ULL << 2)   /* LFENCE always serialises    */
#define CPU_AMD2_SMM_PG_CFG_LOCK (1ULL << 3)
#define CPU_AMD2_NULL_SEL_CLR   (1ULL << 6)   /* null selector clears base   */
#define CPU_AMD2_AUTOIBRS       (1ULL << 8)   /* eIBRS via EFER, no MSR cost */
#define CPU_AMD2_NO_SMM_CTL_MSR (1ULL << 9)
#define CPU_AMD2_SBPB           (1ULL << 27)  /* selective branch pred. bar. */
#define CPU_AMD2_IBPB_BRTYPE    (1ULL << 28)  /* IBPB also flushes branch type*/
#define CPU_AMD2_SRSO_NO        (1ULL << 29)  /* not affected by Inception   */

/* ── Extended Feature Flags (CPUID Leaf 7 Subleaf 1 EDX) ──────────────────── */
#define CPU_EXT1D_AVX_VNNI_INT8   (1ULL << 4)
#define CPU_EXT1D_AVX_NE_CONVERT  (1ULL << 5)
#define CPU_EXT1D_AVX_VNNI_INT16  (1ULL << 10)
#define CPU_EXT1D_PREFETCHI       (1ULL << 14)
#define CPU_EXT1D_AVX10           (1ULL << 19)
#define CPU_EXT1D_APX_F           (1ULL << 21)

/* ── Extended Function Flags (CPUID Leaf 0x80000008 EBX) ──────────────────── */
#define CPU_EXT3_CLZERO         (1ULL << 0)   /* zero a cache line, no RFO   */
#define CPU_EXT3_RETIRED_IP     (1ULL << 1)
#define CPU_EXT3_XSAVEERPTR     (1ULL << 2)
#define CPU_EXT3_RDPRU          (1ULL << 4)
#define CPU_EXT3_MCOMMIT        (1ULL << 8)
#define CPU_EXT3_WBNOINVD       (1ULL << 9)   /* writeback without invalidate*/
#define CPU_EXT3_IBPB           (1ULL << 12)
#define CPU_EXT3_IBRS           (1ULL << 14)
#define CPU_EXT3_STIBP          (1ULL << 15)
#define CPU_EXT3_IBRS_ALWAYS    (1ULL << 16)
#define CPU_EXT3_STIBP_ALWAYS   (1ULL << 17)
#define CPU_EXT3_IBRS_PREFERRED (1ULL << 18)
#define CPU_EXT3_IBRS_SAME_MODE (1ULL << 19)
#define CPU_EXT3_AMD_PPIN       (1ULL << 23)
#define CPU_EXT3_SSBD           (1ULL << 24)
#define CPU_EXT3_SSB_NO         (1ULL << 26)
#define CPU_EXT3_CPPC           (1ULL << 27)
#define CPU_EXT3_AMD_PSFD       (1ULL << 28)

/* ── Extended Function Flags (CPUID Leaf 0x80000007 EDX) ──────────────────── */
#define CPU_PM_INVARIANT_TSC    (1ULL << 8)

/* ── Extended Function Flags (CPUID Leaf 0x80000001 EDX/ECX) ──────────────── */
#define CPU_EXT2_SYSCALL        (1ULL << 11)
#define CPU_EXT2_NX             (1ULL << 20)
#define CPU_EXT2_1GB_PAGE       (1ULL << 26)
#define CPU_EXT2_RDTSCP         (1ULL << 27)
#define CPU_EXT2_LM             (1ULL << 29)
#define CPU_EXT2_LAHF_LM        (1ULL << 32)
#define CPU_EXT2_ABM            (1ULL << 37)
#define CPU_EXT2_SSE4A          (1ULL << 38)
#define CPU_EXT2_3DNOWPREFETCH  (1ULL << 40)
#define CPU_EXT2_TOPOEXT        (1ULL << 54)
#define CPU_EXT2_MWAITX         (1ULL << 61)

/* ── CPU Information & Detected Features Struct ───────────────────────────── */
typedef struct {
    char        vendor_id[16];          /* e.g. "GenuineIntel" / "AuthenticAMD" */
    char        brand_string[64];       /* e.g. "Intel(R) Core(TM) i7..." */
    u32         max_leaf;
    u32         max_ext_leaf;
    u32         family;
    u32         model;
    u32         stepping;
    u32         clflush_size;           /* Cache line flush size in bytes */
    u64         features;               /* Combined CPU_FEAT_* (leaf 1 EDX + ECX) */
    u64         ext_features;           /* Combined CPU_EXT_* (leaf 7 EBX + ECX) */
    u64         ext2_features;          /* Combined CPU_EXT2_* (leaf 0x80000001) */
    u64         xsave_supported_mask;   /* Supported XCR0 components */
    u64         xcr0_active_mask;       /* Currently programmed XCR0 */
    bool        has_xsaveopt;           /* Leaf 0xD Subleaf 1 EAX bit 0 */
    bool        has_pge;                /* Page Global Enable supported */
    bool        has_umip;               /* User-Mode Instruction Prevention supported */
    bool        has_smep;               /* Supervisor Mode Execution Prevention supported */
    bool        has_smap;               /* Supervisor Mode Access Prevention supported */
    bool        has_fsgsbase;           /* FSGSBASE instructions supported */
    bool        has_avx;                /* AVX 256-bit SIMD supported */
    bool        has_avx2;               /* AVX2 integer 256-bit SIMD supported */

    /* ── Additional CPUID leaves ─────────────────────────────────────────── */
    u64         extd_features;          /* CPU_EXTD_* (leaf 7 subleaf 0 EDX)  */
    u64         ext1_features;          /* CPU_EXT1_* (leaf 7 subleaf 1 EAX)  */
    u64         pm_features;            /* CPU_PM_*   (leaf 0x80000007 EDX)   */
    u64         arch_caps;              /* IA32_ARCH_CAPABILITIES, 0 if absent */

    u32         xsave_area_size;        /* Bytes needed for the enabled XCR0   */
    u32         xsave_max_size;         /* Bytes for every supported component */
    u32         phys_addr_bits;         /* MAXPHYADDR (leaf 0x80000008 EAX)    */
    u32         virt_addr_bits;         /* Linear address bits                 */
    u32         tsc_khz;                /* TSC frequency, 0 if not enumerable  */
    u32         base_mhz;               /* Leaf 0x16 base frequency            */
    u32         max_mhz;                /* Leaf 0x16 max turbo frequency       */
    u32         bus_mhz;                /* Leaf 0x16 bus/reference frequency   */
    u32         cache_l1d_kb;           /* Leaf 4 / 0x80000005 cache sizes     */
    u32         cache_l1i_kb;
    u32         cache_l2_kb;
    u32         cache_l3_kb;
    u32         cores_per_package;      /* Physical cores enumerated by leaf 4 */
    u32         threads_per_core;       /* SMT width (leaf 0xB level 0)        */
    u32         mwait_c1_substates;     /* Leaf 5 EDX C1 sub-state count       */

    bool        has_xsavec;             /* Compacted XSAVEC/XRSTOR supported   */
    bool        has_xsaves;             /* Supervisor XSAVES/XRSTORS supported */
    bool        has_xgetbv1;            /* XGETBV with ECX=1 supported         */
    bool        has_pcid;               /* Process-context identifiers         */
    bool        has_invpcid;            /* INVPCID instruction                 */
    bool        has_pku;                /* Memory-protection keys for user     */
    bool        has_erms;               /* Enhanced REP MOVSB/STOSB            */
    bool        has_fsrm;               /* Fast short REP MOVSB                */
    bool        has_monitor;            /* MONITOR/MWAIT available             */
    bool        has_rdtscp;             /* RDTSCP / TSC_AUX                    */
    bool        has_rdpid;              /* RDPID instruction                   */
    bool        has_invariant_tsc;      /* TSC ticks at a constant rate        */
    bool        has_arat;               /* LAPIC timer does not stop in C-states */
    bool        has_avx512f;            /* AVX-512 foundation                  */
    bool        has_1gb_pages;          /* PDPE1GB                             */
    bool        has_x2apic;             /* x2APIC mode available               */
    bool        has_tsc_deadline;       /* LAPIC TSC-deadline timer mode       */

    /* ── Extensions the kernel's own fast paths dispatch on (hwaccel.c) ──── */
    u64         ext1d_features;         /* CPU_EXT1D_* (leaf 7 subleaf 1 EDX) */
    u64         ext3_features;          /* CPU_EXT3_*  (leaf 0x80000008 EBX)  */
    bool        has_sse4_2;             /* CRC32 instruction (GPR operands)   */
    bool        has_popcnt;             /* POPCNT                             */
    bool        has_bmi1;               /* ANDN/BLSR/TZCNT                    */
    bool        has_bmi2;               /* BZHI/PDEP/PEXT                     */
    bool        has_lzcnt;              /* LZCNT (AMD ABM / Intel via BMI1)   */
    bool        has_clflushopt;         /* CLFLUSHOPT                         */
    bool        has_clwb;               /* CLWB                               */
    bool        has_clzero;             /* AMD CLZERO — zero a line, no RFO   */
    bool        has_wbnoinvd;           /* WBNOINVD                           */
    bool        has_movdiri;            /* MOVDIRI  (4/8-byte direct store)   */
    bool        has_movdir64b;          /* MOVDIR64B (64-byte direct store)   */
    bool        has_serialize;          /* SERIALIZE instruction              */
    bool        has_waitpkg;            /* UMONITOR/UMWAIT/TPAUSE             */
    bool        has_cldemote;           /* CLDEMOTE                           */
    bool        has_rdseed;             /* RDSEED                             */
    bool        has_prefetchw;          /* PREFETCHW (3DNowPrefetch)          */
    bool        has_shstk;              /* CET shadow stack                   */
    bool        has_ibt;                /* CET indirect branch tracking       */

    /* ── Words added for the mitigation engine and the MCA/split-lock code ── */
    u64         ext2d_features;         /* CPU_EXT2D_* (leaf 7 subleaf 2 EDX) */
    u64         amd2_features;          /* CPU_AMD2_*  (leaf 0x80000021 EAX)  */
    u64         core_caps;              /* IA32_CORE_CAPABILITIES, 0 if absent */
    u64         mcg_cap;                /* IA32_MCG_CAP, 0 if no MCA          */
    u32         mce_banks;              /* Error-reporting banks MCA exposes  */

    bool        has_rdrand;             /* RDRAND                             */
    bool        has_mce;                /* #MC exception (leaf 1 EDX bit 7)   */
    bool        has_mca;                /* MCG_CAP/bank MSRs (EDX bit 14)     */
    bool        has_core_caps;          /* IA32_CORE_CAPABILITIES readable    */
    bool        has_split_lock_detect;  /* #AC on a split lock is programmable*/
    bool        has_bus_lock_detect;    /* #DB on a bus lock is programmable  */
    bool        has_la57;               /* 5-level paging supported by the CPU*/
    bool        has_lam;                /* Linear address masking             */
    bool        has_pks;                /* Protection keys for supervisor     */
    bool        has_keylocker;          /* AES Key Locker                     */
    bool        has_amx;                /* AMX tile/matrix state              */
    bool        has_hybrid;             /* Heterogeneous (P-core/E-core) part */
    bool        has_md_clear;           /* VERW overwrites microarch buffers  */
    bool        has_l1d_flush;          /* IA32_FLUSH_CMD L1D writeback       */
    bool        has_ibpb;               /* IA32_PRED_CMD indirect-branch bar. */
    bool        has_stibp;              /* Single-thread indirect br. predict */
    bool        has_ssbd;               /* Speculative store bypass disable   */
} cpu_features_t;

/* XSAVE strategy actually chosen at boot; see cpu_xsave_variant(). */
#define XSAVE_VARIANT_FXSAVE   0   /* legacy FXSAVE64 / FXRSTOR64  */
#define XSAVE_VARIANT_XSAVE    1   /* XSAVE64 / XRSTOR64           */
#define XSAVE_VARIANT_XSAVEOPT 2   /* XSAVEOPT64 / XRSTOR64        */
#define XSAVE_VARIANT_XSAVEC   3   /* XSAVEC64 / XRSTOR64 (compact)*/

/* Upper bound on a thread's SIMD save area. Must be >= the XSAVE area size for
 * every component we ever put in XCR0, plus 63 bytes of slack so the runtime
 * 64-byte alignment bump inside the buffer cannot run past its end.
 * x87+SSE+AVX+AVX-512 (XCR0 0xE7) needs 2696 bytes; 3136 leaves headroom and
 * cpu_enable_features_bsp() refuses to enable components that would exceed it. */
#define FPU_STATE_MAX_SIZE  3136

extern cpu_features_t g_cpu_info;
extern u8  g_fsgsbase_enabled;
extern u8  g_smep_enabled;
extern u8  g_smap_enabled;
extern u8  g_osxsave_enabled;
extern u8  g_pge_enabled;
extern u8  g_umip_enabled;
extern u64 g_xcr0_mask;
extern u8  g_pku_enabled;
extern u8  g_pcid_enabled;
extern u8  g_invpcid_enabled;
extern u8  g_erms_enabled;
extern u8  g_mwait_idle_enabled;
extern u8  g_xsave_variant;         /* XSAVE_VARIANT_* */
extern u32 g_xsave_area_size;       /* Bytes XSAVE/XSAVEC writes for XCR0 */
extern u8  g_split_lock_detect;     /* #AC-on-split-lock currently armed */

/* CR4 bits that must be set on every core for the whole life of the system.
 * cpu_write_cr4() ORs them back into every write, so a corrupted or
 * attacker-influenced CR4 value cannot be the thing that turns SMEP off. */
extern u64 g_cr4_pinned;

/** Detect all CPU hardware extensions via CPUID */
void cpu_detect_features(void);

/**
 * cpu_write_cr4(val) — write CR4 with the pinned protection bits forced on.
 *
 * Every CR4 write in the kernel goes through this. SMEP, SMAP, UMIP and
 * FSGSBASE are set once at boot and never legitimately cleared afterwards, so
 * a write that would clear one is either a bug or a write-primitive being used
 * to disarm the protection before a ret2usr — in both cases the right answer
 * is to put the bit back and say so, not to honour the value.
 */
void cpu_write_cr4(u64 val);

/**
 * cpu_pin_control_regs() — freeze the current CR4 protection bits.
 *
 * Called on the BSP once the boot-time feature decisions are final. Everything
 * set in CR4 at that moment out of the pinnable set becomes mandatory.
 */
void cpu_pin_control_regs(void);

/**
 * cpu_check_control_regs(who) — audit CR0.WP, CR4's pinned bits and CR4.PCE.
 *
 * Returns true when the control registers still hold the boot-time policy.
 * Anything wrong is repaired in place and logged with @who naming the caller,
 * because a kernel that keeps running with SMEP off is a worse outcome than a
 * noisy log line.
 */
bool cpu_check_control_regs(const char *who);

/**
 * cpu_rand64(out) — one 64-bit value from the CPU's entropy source.
 *
 * Prefers RDSEED (a true conditioned entropy sample) and falls back to RDRAND
 * (the DRBG seeded from it), retrying each a bounded number of times because
 * both are architecturally permitted to fail when their pool is momentarily
 * drained. Returns false when the CPU has neither, or when every attempt
 * failed — callers must have a plan for that rather than using *out.
 */
bool cpu_rand64(u64 *out);

/** Enable architecture CPU extensions on the Bootstrap Processor (BSP) */
void cpu_enable_features_bsp(void);

/** Enable architecture CPU extensions on an Application Processor (AP) */
void cpu_enable_features_ap(void);

/** Check if a specific standard feature is supported */
static inline bool cpu_has_feature(u64 feat) {
    return (g_cpu_info.features & feat) != 0;
}

/** Check if a specific extended feature is supported */
static inline bool cpu_has_ext_feature(u64 feat) {
    return (g_cpu_info.ext_features & feat) != 0;
}

/** Check a leaf-7 subleaf-0 EDX feature (CPU_EXTD_*) */
static inline bool cpu_has_extd_feature(u64 feat) {
    return (g_cpu_info.extd_features & feat) != 0;
}

/** Format the flags list for /proc/cpuinfo into @buf */
size_t cpu_format_flags(char *buf, size_t max);

/** Format the "bugs" list (unmitigated CPU errata) for /proc/cpuinfo. */
size_t cpu_format_bugs(char *buf, size_t max);

/** Check a leaf-7 subleaf-2 EDX feature (CPU_EXT2D_*) */
static inline bool cpu_has_ext2d_feature(u64 feat) {
    return (g_cpu_info.ext2d_features & feat) != 0;
}

/** Check a leaf-0x80000021 EAX feature (CPU_AMD2_*) */
static inline bool cpu_has_amd2_feature(u64 feat) {
    return (g_cpu_info.amd2_features & feat) != 0;
}

/** True on parts that report "GenuineIntel" / "AuthenticAMD" respectively. */
bool cpu_is_intel(void);
bool cpu_is_amd(void);

/**
 * cpu_arm_split_lock_detect() / cpu_disarm_split_lock_detect()
 *
 * Program IA32_TEST_CTRL so a locked access spanning two cache lines raises
 * #AC instead of silently taking the bus lock. The MSR is per-logical-
 * processor, so both are called on every core. No-ops where the part does not
 * enumerate the capability in IA32_CORE_CAPABILITIES.
 */
void cpu_arm_split_lock_detect(void);
void cpu_disarm_split_lock_detect(void);

/**
 * cpu_split_lock_fault(rip, from_user) — account one split-lock #AC.
 *
 * Called by the #AC path in idt.c once it has decided the fault was a split
 * lock rather than a userspace alignment check. Logs the first few offenders
 * and then disarms detection on this core so the retried instruction makes
 * progress: the point is to name the code doing it, not to make the machine
 * unusable for software that was already shipping.
 */
void cpu_split_lock_fault(u64 rip, bool from_user);

/** Number of split-lock faults seen since boot (for /proc reporting). */
u64 cpu_split_lock_count(void);

/**
 * cpu_idle_arm(addr) / cpu_idle_wait() — park the calling CPU until either an
 * interrupt arrives or someone stores to @addr.
 *
 * Call them around the "is there work?" test, in this order:
 *
 *     cpu_cli();
 *     cpu_idle_arm(&work_flag);
 *     if (work_flag) { cpu_sti(); continue; }
 *     cpu_idle_wait();
 *
 * Arming before the test is what makes it race-free: a store landing after the
 * test still breaks the MWAIT. Where MONITOR/MWAIT is unavailable both degrade
 * to the STI;HLT idiom, which is race-free for its own reason (STI's shadow
 * covers the HLT). Both must be called with interrupts disabled;
 * cpu_idle_wait() re-enables them.
 */
void cpu_idle_arm(const volatile void *monitor_addr);
void cpu_idle_wait(void);
