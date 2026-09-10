/* ============================================================================
 * AzamiOS — MSR & CPUID Helpers (x86_64)
 * File: arch/x86_64/cpu/msr.h
 * ============================================================================ */
#pragma once

#include "../../../include/azami/types.h"

/* ── Well-known MSR addresses ─────────────────────────────────────────────── */
#define MSR_EFER          0xC0000080UL  /* Extended Feature Enable Register */
#define MSR_STAR          0xC0000081UL  /* Syscall target CS/SS selectors */
#define MSR_LSTAR         0xC0000082UL  /* Syscall handler RIP (64-bit mode) */
#define MSR_CSTAR         0xC0000083UL  /* Syscall handler RIP (compat mode) */
#define MSR_SFMASK        0xC0000084UL  /* RFLAGS bits to clear on SYSCALL */
#define MSR_FS_BASE       0xC0000100UL  /* FS segment base (user TLS) */
#define MSR_GS_BASE       0xC0000101UL  /* GS segment base (kernel per-CPU) */
#define MSR_KERNEL_GS_BASE 0xC0000102UL /* GS base after SWAPGS */
#define MSR_TSC_AUX       0xC0000103UL  /* RDTSCP/RDPID auxiliary value */
#define MSR_APIC_BASE     0x0000001BUL  /* APIC base address */
#define MSR_PAT           0x00000277UL  /* Page Attribute Table */
#define MSR_IA32_XSS      0x00000DA0UL  /* XSAVES supervisor state mask */
#define MSR_IA32_TSC_DEADLINE 0x000006E0UL /* LAPIC TSC-deadline target */
#define MSR_IA32_MISC_ENABLE  0x000001A0UL
#define MSR_IA32_UMWAIT_CONTROL 0x000000E1UL

/* Speculative-execution control (Spectre/Meltdown class mitigations) */
#define MSR_IA32_SPEC_CTRL        0x00000048UL
#define MSR_IA32_PRED_CMD         0x00000049UL
#define MSR_IA32_ARCH_CAPABILITIES 0x0000010AUL
#define MSR_IA32_FLUSH_CMD        0x0000010BUL

#define SPEC_CTRL_IBRS   (1ULL << 0)
#define SPEC_CTRL_STIBP  (1ULL << 1)
#define SPEC_CTRL_SSBD   (1ULL << 2)
#define PRED_CMD_IBPB    (1ULL << 0)
#define FLUSH_CMD_L1D    (1ULL << 0)

/* IA32_ARCH_CAPABILITIES bits that tell us a mitigation is unnecessary */
#define ARCH_CAP_RDCL_NO      (1ULL << 0)  /* not vulnerable to Meltdown  */
#define ARCH_CAP_IBRS_ALL     (1ULL << 1)  /* enhanced IBRS: set once     */
#define ARCH_CAP_SKIP_VMENTRY_L1DFLUSH (1ULL << 3)
#define ARCH_CAP_SSB_NO       (1ULL << 4)  /* not vulnerable to Spectre-v4 */
#define ARCH_CAP_MDS_NO       (1ULL << 5)
#define ARCH_CAP_TAA_NO       (1ULL << 8)

/* EFER bit definitions */
#define EFER_SCE  (1ULL << 0)   /* SYSCALL Enable */
#define EFER_LME  (1ULL << 8)   /* Long Mode Enable */
#define EFER_LMA  (1ULL << 10)  /* Long Mode Active (read-only) */
#define EFER_NXE  (1ULL << 11)  /* No-Execute Enable */

/* ── Inline RDMSR / WRMSR ─────────────────────────────────────────────────── */

static __attribute__((always_inline)) inline u64 rdmsr(u32 msr)
{
    u32 lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((u64)hi << 32) | lo;
}

static __attribute__((always_inline)) inline void wrmsr(u32 msr, u64 val)
{
    u32 lo = (u32)(val & 0xFFFFFFFFUL);
    u32 hi = (u32)(val >> 32);
    __asm__ volatile("wrmsr" : : "c"(msr), "a"(lo), "d"(hi) : "memory");
}

/* ── CPUID wrapper ─────────────────────────────────────────────────────────── */

static inline void cpuid(u32 leaf, u32 subleaf,
                          u32 *eax, u32 *ebx, u32 *ecx, u32 *edx)
{
    __asm__ volatile("cpuid"
                     : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                     : "a"(leaf),  "c"(subleaf));
}

/* Convenience: check if CPUID leaf is available */
static inline bool cpuid_has_leaf(u32 leaf)
{
    u32 max_leaf, b, c, d;
    cpuid(0, 0, &max_leaf, &b, &c, &d);
    return leaf <= max_leaf;
}

/* ── CR register accessors ─────────────────────────────────────────────────── */

static __attribute__((always_inline)) inline u64 read_cr0(void) {
    u64 v; __asm__ volatile("mov %%cr0, %0" : "=r"(v)); return v;
}
static __attribute__((always_inline)) inline void write_cr0(u64 v) {
    __asm__ volatile("mov %0, %%cr0" : : "r"(v) : "memory");
}
static __attribute__((always_inline)) inline u64 read_cr2(void) {
    u64 v; __asm__ volatile("mov %%cr2, %0" : "=r"(v)); return v;
}
static __attribute__((always_inline)) inline u64 read_cr3(void) {
    u64 v; __asm__ volatile("mov %%cr3, %0" : "=r"(v)); return v;
}
static __attribute__((always_inline)) inline void write_cr3(u64 v) {
    __asm__ volatile("mov %0, %%cr3" : : "r"(v) : "memory");
}
static __attribute__((always_inline)) inline u64 read_cr4(void) {
    u64 v; __asm__ volatile("mov %%cr4, %0" : "=r"(v)); return v;
}
static __attribute__((always_inline)) inline void write_cr4(u64 v) {
    __asm__ volatile("mov %0, %%cr4" : : "r"(v) : "memory");
}

/* ── Extended Control Register (XCR) ──────────────────────────────────────── */
static __attribute__((always_inline)) inline u64 xgetbv(u32 index) {
    u32 eax, edx;
    __asm__ volatile("xgetbv" : "=a"(eax), "=d"(edx) : "c"(index));
    return ((u64)edx << 32) | eax;
}
static __attribute__((always_inline)) inline void xsetbv(u32 index, u64 value) {
    u32 eax = (u32)(value & 0xFFFFFFFF);
    u32 edx = (u32)(value >> 32);
    __asm__ volatile("xsetbv" : : "a"(eax), "d"(edx), "c"(index) : "memory");
}


/* ── TLB invalidation ─────────────────────────────────────────────────────── */
static __attribute__((always_inline)) inline void invlpg(uintptr_t va) {
    __asm__ volatile("invlpg (%0)" : : "r"(va) : "memory");
}
static __attribute__((always_inline)) inline void tlb_flush_all(void) {
    write_cr3(read_cr3());  /* Reload CR3 to flush non-global TLB entries */
}

/* ── FSGSBASE Instructions ────────────────────────────────────────────────── */
static __attribute__((always_inline)) inline u64 rdfsbase(void) {
    u64 v;
    __asm__ volatile("rdfsbase %0" : "=r"(v));
    return v;
}
static __attribute__((always_inline)) inline void wrfsbase(u64 v) {
    __asm__ volatile("wrfsbase %0" : : "r"(v) : "memory");
}
static __attribute__((always_inline)) inline u64 rdgsbase(void) {
    u64 v;
    __asm__ volatile("rdgsbase %0" : "=r"(v));
    return v;
}
static __attribute__((always_inline)) inline void wrgsbase(u64 v) {
    __asm__ volatile("wrgsbase %0" : : "r"(v) : "memory");
}

/* ── Timestamp & Cache Extensions ─────────────────────────────────────────── */
static __attribute__((always_inline)) inline u64 rdtsc(void) {
    u32 lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}
static __attribute__((always_inline)) inline u64 rdtscp(u32 *aux) {
    u32 lo, hi, a;
    __asm__ volatile("rdtscp" : "=a"(lo), "=d"(hi), "=c"(a));
    if (aux) *aux = a;
    return ((u64)hi << 32) | lo;
}
static __attribute__((always_inline)) inline void clflushopt(const void *p) {
    __asm__ volatile("clflushopt (%0)" : : "r"(p) : "memory");
}
static __attribute__((always_inline)) inline void clwb(const void *p) {
    __asm__ volatile("clwb (%0)" : : "r"(p) : "memory");
}

/* ── PCID / INVPCID ───────────────────────────────────────────────────────── */
#define INVPCID_ADDR        0  /* one address in one PCID                 */
#define INVPCID_SINGLE_CTX  1  /* every non-global entry of one PCID      */
#define INVPCID_ALL_GLOBAL  2  /* every entry of every PCID, globals too  */
#define INVPCID_ALL_NONGLOB 3  /* every non-global entry of every PCID    */

struct invpcid_desc { u64 pcid; u64 addr; };

static __attribute__((always_inline)) inline void invpcid(u64 type, u64 pcid, u64 addr)
{
    struct invpcid_desc d = { pcid, addr };
    __asm__ volatile("invpcid %1, %0" : : "r"(type), "m"(d) : "memory");
}

/* ── MONITOR / MWAIT ──────────────────────────────────────────────────────── */
static __attribute__((always_inline)) inline void monitor_op(const void *addr, u32 ext, u32 hints)
{
    __asm__ volatile("monitor" : : "a"(addr), "c"(ext), "d"(hints) : "memory");
}

/* MWAIT hint: bits 7:4 select the C-state (0 => C1), bit 0 requests that
 * interrupts break the wait even when masked. */
static __attribute__((always_inline)) inline void mwait_op(u32 hints, u32 ext)
{
    __asm__ volatile("mwait" : : "a"(hints), "c"(ext) : "memory");
}

/* ── RDPID (leaf 7.1 ECX bit 22) ──────────────────────────────────────────── */
static __attribute__((always_inline)) inline u64 rdpid(void)
{
    u64 v;
    __asm__ volatile("rdpid %0" : "=r"(v));
    return v;
}

/* ── Memory-protection keys (PKU): the PKRU is a register, not an MSR ─────── */
static __attribute__((always_inline)) inline u32 rdpkru(void)
{
    u32 eax, edx;
    __asm__ volatile("rdpkru" : "=a"(eax), "=d"(edx) : "c"(0));
    (void)edx;
    return eax;
}

static __attribute__((always_inline)) inline void wrpkru(u32 val)
{
    __asm__ volatile("wrpkru" : : "a"(val), "c"(0), "d"(0) : "memory");
}

/* ── Cache maintenance ────────────────────────────────────────────────────── */
static __attribute__((always_inline)) inline void clflush(const void *p) {
    __asm__ volatile("clflush (%0)" : : "r"(p) : "memory");
}

/* ── Supervisor Mode Access Prevention (SMAP) Overrides ───────────────────── */
static __attribute__((always_inline)) inline void stac(void) {
    __asm__ volatile("stac" : : : "cc");
}
static __attribute__((always_inline)) inline void clac(void) {
    __asm__ volatile("clac" : : : "cc");
}


/* ── Control-register bits ────────────────────────────────────────────────── *
 * Defined here rather than in cpu.c so the feature-enablement code, the
 * mitigation policy engine and the control-register pinning audit all name the
 * same bit. A second private copy of these is how a hardening check ends up
 * verifying a different bit from the one that was set.
 * ------------------------------------------------------------------------- */
#define CR0_MP         (1ULL << 1)
#define CR0_EM         (1ULL << 2)
#define CR0_TS         (1ULL << 3)
#define CR0_NE         (1ULL << 5)
#define CR0_WP         (1ULL << 16)
#define CR0_AM         (1ULL << 18)
#define CR0_NW         (1ULL << 29)
#define CR0_CD         (1ULL << 30)

#define CR4_DE         (1ULL << 3)
#define CR4_PSE        (1ULL << 4)
#define CR4_PAE        (1ULL << 5)
#define CR4_MCE        (1ULL << 6)   /* machine-check exception enable      */
#define CR4_PGE        (1ULL << 7)
#define CR4_PCE        (1ULL << 8)   /* RDPMC from ring 3 — must stay clear */
#define CR4_OSFXSR     (1ULL << 9)
#define CR4_OSXMMEXCPT (1ULL << 10)
#define CR4_UMIP       (1ULL << 11)
#define CR4_LA57       (1ULL << 12)
#define CR4_FSGSBASE   (1ULL << 16)
#define CR4_PCIDE      (1ULL << 17)
#define CR4_OSXSAVE    (1ULL << 18)
#define CR4_SMEP       (1ULL << 20)
#define CR4_SMAP       (1ULL << 21)
#define CR4_PKE        (1ULL << 22)
#define CR4_CET        (1ULL << 23)  /* control-flow enforcement            */
#define CR4_PKS        (1ULL << 24)  /* protection keys for supervisor      */

/* ── Machine-check architecture ───────────────────────────────────────────── */
#define MSR_IA32_MCG_CAP      0x00000179UL
#define MSR_IA32_MCG_STATUS   0x0000017AUL
#define MSR_IA32_MCG_CTL      0x0000017BUL
#define MSR_IA32_MCG_EXT_CTL  0x000004D0UL

/* Per-bank register block: four MSRs per bank starting at 0x400. */
#define MSR_IA32_MCx_CTL(b)    (0x00000400UL + 4UL * (b))
#define MSR_IA32_MCx_STATUS(b) (0x00000401UL + 4UL * (b))
#define MSR_IA32_MCx_ADDR(b)   (0x00000402UL + 4UL * (b))
#define MSR_IA32_MCx_MISC(b)   (0x00000403UL + 4UL * (b))

#define MCG_CAP_COUNT_MASK    0xFFULL       /* number of error-reporting banks */
#define MCG_CAP_CTL_P         (1ULL << 8)   /* IA32_MCG_CTL present            */
#define MCG_CAP_EXT_P         (1ULL << 9)
#define MCG_CAP_CMCI_P        (1ULL << 10)  /* corrected-error interrupt       */
#define MCG_CAP_TES_P         (1ULL << 11)  /* threshold-based error status    */
#define MCG_CAP_SER_P         (1ULL << 24)  /* software error recovery         */
#define MCG_CAP_LMCE_P        (1ULL << 27)  /* local machine-check exception   */

#define MCG_STATUS_RIPV       (1ULL << 0)   /* pushed RIP is restartable       */
#define MCG_STATUS_EIPV       (1ULL << 1)   /* pushed RIP is the erring insn   */
#define MCG_STATUS_MCIP       (1ULL << 2)   /* a #MC is in progress            */
#define MCG_STATUS_LMCE_S     (1ULL << 3)

#define MCI_STATUS_VAL        (1ULL << 63)  /* bank holds a valid record       */
#define MCI_STATUS_OVER       (1ULL << 62)  /* a second error was lost         */
#define MCI_STATUS_UC         (1ULL << 61)  /* uncorrected                     */
#define MCI_STATUS_EN         (1ULL << 60)  /* reporting was enabled           */
#define MCI_STATUS_MISCV      (1ULL << 59)  /* IA32_MCi_MISC is valid          */
#define MCI_STATUS_ADDRV      (1ULL << 58)  /* IA32_MCi_ADDR is valid          */
#define MCI_STATUS_PCC        (1ULL << 57)  /* processor context corrupt       */
#define MCI_STATUS_S          (1ULL << 56)  /* signalled (SER parts)           */
#define MCI_STATUS_AR         (1ULL << 55)  /* action required (SER parts)     */

/* ── Split-lock / bus-lock detection ──────────────────────────────────────── */
#define MSR_IA32_CORE_CAPABILITIES 0x000000CFUL
#define CORE_CAP_SPLIT_LOCK_DETECT (1ULL << 5)

#define MSR_IA32_TEST_CTRL         0x00000033UL
#define TEST_CTRL_SPLIT_LOCK_AC    (1ULL << 29)  /* #AC on a split lock */

#define MSR_IA32_DEBUGCTL          0x000001D9UL

/* ── Performance monitoring counters ─────────────────────────────────────── */
/* Intel architectural PMU (CPUID leaf 0xA). One PERFEVTSEL selects what a
 * counter counts; the counter itself is read either through its MSR or, far
 * more cheaply, with RDPMC on the counter's index. From version 2 onwards a
 * counter also has to be armed in IA32_PERF_GLOBAL_CTRL before it will tick. */
#define MSR_IA32_PMC0              0x000000C1UL
#define MSR_IA32_PERFEVTSEL0       0x00000186UL
#define MSR_IA32_FIXED_CTR0        0x00000309UL
#define MSR_IA32_FIXED_CTR_CTRL    0x0000038DUL
#define MSR_IA32_PERF_GLOBAL_STATUS   0x0000038EUL
#define MSR_IA32_PERF_GLOBAL_CTRL     0x0000038FUL
#define MSR_IA32_PERF_GLOBAL_OVF_CTRL 0x00000390UL

/* AMD: four legacy K7/K8 counters, and six wider ones on parts that enumerate
 * PerfCtrExtCore (CPUID 0x80000001 ECX bit 23). The extended pair is strided
 * by two, so counter i lives at BASE + 2*i. */
#define MSR_K7_PERFEVTSEL0         0xC0010000UL
#define MSR_K7_PERFCTR0            0xC0010004UL
#define MSR_AMD_PERFEVTSEL_EXT0    0xC0010200UL
#define MSR_AMD_PERFCTR_EXT0       0xC0010201UL
#define DEBUGCTL_BUS_LOCK_DETECT   (1ULL << 2)   /* #DB on a bus lock   */

/* ── Transactional-memory disable (the TAA mitigation) ────────────────────── */
#define MSR_IA32_TSX_CTRL          0x00000122UL
#define TSX_CTRL_RTM_DISABLE       (1ULL << 0)
#define TSX_CTRL_CPUID_CLEAR       (1ULL << 1)   /* hide HLE/RTM from CPUID */

/* ── Control-flow enforcement (CET) ───────────────────────────────────────── *
 * Enumerated and reported; not programmed. Turning CET on means every indirect
 * branch target in the kernel needs an ENDBR64 (a -fcf-protection build) and
 * every kernel stack needs a shadow stack allocated beside it — until both
 * exist, setting these would fault the first time an interrupt returned.
 * ------------------------------------------------------------------------- */
#define MSR_IA32_U_CET             0x000006A0UL
#define MSR_IA32_S_CET             0x000006A2UL
#define MSR_IA32_PL0_SSP           0x000006A4UL
#define MSR_IA32_PL3_SSP           0x000006A7UL
#define MSR_IA32_INT_SSP_TAB       0x000006A8UL
#define CET_SHSTK_EN               (1ULL << 0)
#define CET_ENDBR_EN               (1ULL << 2)

/* ── AMD-specific control MSRs ────────────────────────────────────────────── */
#define MSR_AMD64_LS_CFG           0xC0011020UL  /* non-arch SSBD (fam 15h-17h)*/
#define MSR_AMD64_DE_CFG           0xC0011029UL  /* LFENCE serialising bit     */
#define MSR_AMD64_VIRT_SPEC_CTRL   0xC001011FUL

#define DE_CFG_LFENCE_SERIALIZE    (1ULL << 1)

/* ── Speculation control: the bits added after the original IBRS/STIBP ────── */
#define SPEC_CTRL_IPRED_DIS_U      (1ULL << 3)
#define SPEC_CTRL_IPRED_DIS_S      (1ULL << 4)
#define SPEC_CTRL_RRSBA_DIS_U      (1ULL << 5)
#define SPEC_CTRL_RRSBA_DIS_S      (1ULL << 6)
#define SPEC_CTRL_PSFD             (1ULL << 7)  /* predictive store fwd off */
#define SPEC_CTRL_BHI_DIS_S        (1ULL << 10)

#define EFER_AUTOIBRS              (1ULL << 21) /* AMD: eIBRS without the MSR */

/* ── IA32_ARCH_CAPABILITIES: the rest of the "you are not affected" bits ──── */
#define ARCH_CAP_RSBA              (1ULL << 2)  /* RSB alternates to BTB      */
#define ARCH_CAP_PSCHANGE_MC_NO    (1ULL << 6)
#define ARCH_CAP_TSX_CTRL          (1ULL << 7)  /* IA32_TSX_CTRL exists       */
#define ARCH_CAP_MISC_PACKAGE_CTLS (1ULL << 10)
#define ARCH_CAP_ENERGY_FILTERING  (1ULL << 11)
#define ARCH_CAP_DOITM             (1ULL << 12)
#define ARCH_CAP_SBDR_SSDP_NO      (1ULL << 13)
#define ARCH_CAP_FBSDP_NO          (1ULL << 14)
#define ARCH_CAP_PSDP_NO           (1ULL << 15)
#define ARCH_CAP_FB_CLEAR          (1ULL << 17) /* VERW also clears fill bufs */
#define ARCH_CAP_FB_CLEAR_CTRL     (1ULL << 18)
#define ARCH_CAP_RRSBA             (1ULL << 19)
#define ARCH_CAP_BHI_NO            (1ULL << 20)
#define ARCH_CAP_XAPIC_DISABLE     (1ULL << 21)
#define ARCH_CAP_PBRSB_NO          (1ULL << 24)
#define ARCH_CAP_GDS_CTRL          (1ULL << 25)
#define ARCH_CAP_GDS_NO            (1ULL << 26)
#define ARCH_CAP_RFDS_NO           (1ULL << 27)
#define ARCH_CAP_RFDS_CLEAR        (1ULL << 28)

/* ── Fences & serialisation ───────────────────────────────────────────────── */
static __attribute__((always_inline)) inline void lfence(void) {
    __asm__ volatile("lfence" ::: "memory");
}
static __attribute__((always_inline)) inline void sfence(void) {
    __asm__ volatile("sfence" ::: "memory");
}
static __attribute__((always_inline)) inline void mfence(void) {
    __asm__ volatile("mfence" ::: "memory");
}

/* ── Hardware entropy ─────────────────────────────────────────────────────── *
 * Both instructions report failure in CF rather than faulting, and both are
 * documented as allowed to fail transiently — a caller that ignores the carry
 * flag silently accepts whatever was left in the destination register, which
 * on a failing RDSEED is zero. Every use goes through these two.
 * ------------------------------------------------------------------------- */
static __attribute__((always_inline)) inline bool rdrand64_step(u64 *out) {
    unsigned char ok;
    __asm__ volatile("rdrand %0; setc %1" : "=r"(*out), "=qm"(ok) :: "cc");
    return ok != 0;
}
static __attribute__((always_inline)) inline bool rdseed64_step(u64 *out) {
    unsigned char ok;
    __asm__ volatile("rdseed %0; setc %1" : "=r"(*out), "=qm"(ok) :: "cc");
    return ok != 0;
}
