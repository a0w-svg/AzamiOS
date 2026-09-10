/* ============================================================================
 * AzamiOS — Machine Check Architecture (x86_64)
 * File: arch/x86_64/cpu/mce.c
 *
 * See mce.h for what MCA is for. Three things about the implementation are
 * worth stating up front, because each is a place a naive version goes wrong:
 *
 *   - Bank 0 on early P6-family parts reports spurious errors at boot, which is
 *     why Linux leaves its CTL alone there. We do the same rather than start
 *     the system by panicking on a 20-year-old erratum.
 *
 *   - MCi_STATUS must be cleared after reading, and IA32_MCG_STATUS must be
 *     cleared before returning from #MC. A machine check that leaves MCIP set
 *     turns the *next* one into a shutdown, so this is not optional tidying.
 *
 *   - #MC is delivered on IST3 (see idt.c), so the handler runs on its own
 *     stack. It therefore must not assume the interrupted stack was usable —
 *     an uncorrectable error in the kernel stack itself is precisely the case
 *     the dedicated stack exists for.
 * ============================================================================ */

#include "mce.h"
#include "cpu.h"
#include "msr.h"
#include "../../../include/azami/defs.h"
#include "../../../kernel/lib/string.h"

extern void kprintf(const char *fmt, ...);
extern int  scnprintf(char *buf, size_t size, const char *fmt, ...);

u8 g_mce_enabled = 0;

/* Banks the BSP decided to arm. APs replay this rather than re-deriving it, so
 * a hybrid part's E-cores cannot end up watching a different set. */
static u32 s_banks      = 0;
static u32 s_skip_bank0 = 0;

/* ── Error log ───────────────────────────────────────────────────────────── *
 * A small fixed ring. Machine checks are rare enough that 32 records is more
 * history than any real incident produces, and a fixed array is the only kind
 * of allocation that is safe from inside the #MC handler. */
#define MCE_LOG_ENTRIES 32

typedef struct {
    u64 status;
    u64 addr;
    u64 misc;
    u64 mcg_status;
    u64 rip;
    u32 bank;
    u8  corrected;
    u8  valid;
} mce_record_t;

static mce_record_t s_log[MCE_LOG_ENTRIES];
static u32 s_log_head  = 0;   /* next slot to write */
static u32 s_log_total = 0;   /* records ever logged */

static void mce_log(u32 bank, u64 status, u64 addr, u64 misc,
                    u64 mcg_status, u64 rip, bool corrected)
{
    u32 slot = __atomic_fetch_add(&s_log_head, 1, __ATOMIC_RELAXED) % MCE_LOG_ENTRIES;
    mce_record_t *e = &s_log[slot];

    e->valid      = 0;                 /* torn record beats a plausible lie */
    e->bank       = bank;
    e->status     = status;
    e->addr       = addr;
    e->misc       = misc;
    e->mcg_status = mcg_status;
    e->rip        = rip;
    e->corrected  = corrected ? 1 : 0;
    __atomic_store_n(&e->valid, 1, __ATOMIC_RELEASE);
    __atomic_add_fetch(&s_log_total, 1, __ATOMIC_RELAXED);
}

/* ── Bank setup ──────────────────────────────────────────────────────────── */

static void mce_arm_banks(void)
{
    /* Enable reporting for every error type the part knows about, then clear
     * whatever the firmware or a previous OS left behind: a stale VAL bit
     * would be reported as a fresh error on the first poll. */
    for (u32 b = 0; b < s_banks; b++) {
        if (b == 0 && s_skip_bank0) continue;
        wrmsr(MSR_IA32_MCx_CTL(b), ~0ULL);
        wrmsr(MSR_IA32_MCx_STATUS(b), 0);
    }

    if (g_cpu_info.mcg_cap & MCG_CAP_CTL_P)
        wrmsr(MSR_IA32_MCG_CTL, ~0ULL);

    /* A machine check may have been in progress when we took over. */
    wrmsr(MSR_IA32_MCG_STATUS, 0);

    cpu_write_cr4(read_cr4() | CR4_MCE);
}

void mce_init(void)
{
    if (!g_cpu_info.has_mce) {
        kprintf("[MCE] Not supported by this CPU\n");
        return;
    }

    if (!g_cpu_info.has_mca || g_cpu_info.mce_banks == 0) {
        /* #MC without banks: the exception still fires, and taking it is far
         * better than the triple fault an unhandled vector 18 would become. */
        cpu_write_cr4(read_cr4() | CR4_MCE);
        g_mce_enabled = 1;
        kprintf("[MCE] #MC enabled; no MCA banks to decode from\n");
        return;
    }

    s_banks = g_cpu_info.mce_banks;
    if (s_banks > 64) s_banks = 64;   /* MCG_CAP caps at 255; be conservative */

    /* Pentium Pro through early Pentium II log spurious external-bus errors in
     * bank 0. Leaving its CTL at the reset value is what every production OS
     * does on those parts. */
    s_skip_bank0 = (cpu_is_intel() && g_cpu_info.family == 6 &&
                    g_cpu_info.model < 0x1A) ? 1 : 0;

    mce_arm_banks();
    g_mce_enabled = 1;

    kprintf("[MCE] %u banks armed%s%s%s\n", s_banks,
            s_skip_bank0 ? " (bank 0 skipped: P6 erratum)" : "",
            (g_cpu_info.mcg_cap & MCG_CAP_SER_P)  ? ", SER" : "",
            (g_cpu_info.mcg_cap & MCG_CAP_CMCI_P) ? ", CMCI-capable" : "");

    /* Errors logged before we owned the machine are worth seeing once. */
    mce_poll();
}

void mce_init_ap(void)
{
    if (!g_mce_enabled) return;
    if (s_banks == 0) {
        cpu_write_cr4(read_cr4() | CR4_MCE);
        return;
    }
    mce_arm_banks();
}

/* ── Decoding ────────────────────────────────────────────────────────────── */

/* The compound error code in MCi_STATUS[15:0]. Only the shapes that actually
 * tell an operator something are named; the rest are reported as their raw
 * code, which is more honest than a wrong guess. */
static const char *mce_error_type(u64 status)
{
    u16 mcacod = (u16)(status & 0xFFFF);

    if (mcacod == 0x0000) return "no error";
    if (mcacod == 0x0001) return "unclassified";
    if (mcacod == 0x0002) return "microcode ROM parity";
    if (mcacod == 0x0003) return "external error";
    if (mcacod == 0x0004) return "FRC error";
    if (mcacod == 0x0005) return "internal parity";
    if ((mcacod & 0xFFF0) == 0x0010) return "internal timer";
    if ((mcacod & 0xFF00) == 0x0100) return "internal unclassified";
    if ((mcacod & 0xF000) == 0x1000) return "memory hierarchy";
    if ((mcacod & 0xFF00) == 0x0800) return "bus/interconnect";
    if ((mcacod & 0xFFF0) == 0x0150) return "TLB error";
    if ((mcacod & 0xFF00) == 0x0400) return "cache hierarchy";
    return "unrecognised";
}

static void mce_print(u32 bank, u64 status, u64 addr, u64 misc, bool corrected)
{
    kprintf("[MCE] bank %u: %s %s error, status=0x%016llx (%s%s%s%s)\n",
            bank, corrected ? "corrected" : "UNCORRECTED",
            mce_error_type(status), (unsigned long long)status,
            (status & MCI_STATUS_OVER) ? "overflow " : "",
            (status & MCI_STATUS_PCC)  ? "context-corrupt " : "",
            (status & MCI_STATUS_S)    ? "signalled " : "",
            (status & MCI_STATUS_AR)   ? "action-required" : "recoverable");
    if (status & MCI_STATUS_ADDRV)
        kprintf("[MCE]   address=0x%016llx\n", (unsigned long long)addr);
    if (status & MCI_STATUS_MISCV)
        kprintf("[MCE]   misc=0x%016llx\n", (unsigned long long)misc);
}

/* ── The #MC handler ─────────────────────────────────────────────────────── */

bool mce_handle(pt_regs_t *r)
{
    u64 mcg_status = g_cpu_info.has_mca ? rdmsr(MSR_IA32_MCG_STATUS) : 0;
    bool fatal = false;

    kprintf("[MCE] Machine check on RIP=0x%016llx MCG_STATUS=0x%llx%s%s\n",
            (unsigned long long)(r ? r->rip : 0),
            (unsigned long long)mcg_status,
            (mcg_status & MCG_STATUS_RIPV) ? " RIPV" : " !RIPV",
            (mcg_status & MCG_STATUS_EIPV) ? " EIPV" : "");

    for (u32 b = 0; b < s_banks; b++) {
        u64 status = rdmsr(MSR_IA32_MCx_STATUS(b));
        if (!(status & MCI_STATUS_VAL)) continue;

        u64 addr = (status & MCI_STATUS_ADDRV) ? rdmsr(MSR_IA32_MCx_ADDR(b)) : 0;
        u64 misc = (status & MCI_STATUS_MISCV) ? rdmsr(MSR_IA32_MCx_MISC(b)) : 0;
        bool uc  = (status & MCI_STATUS_UC) != 0;

        mce_print(b, status, addr, misc, !uc);
        mce_log(b, status, addr, misc, mcg_status, r ? r->rip : 0, !uc);

        /* PCC means the architectural state itself is untrustworthy — there is
         * nothing left to return to. An uncorrected error without PCC is
         * survivable only if the CPU also says the pushed RIP is restartable. */
        if (status & MCI_STATUS_PCC) fatal = true;
        if (uc && !(mcg_status & MCG_STATUS_RIPV)) fatal = true;

        wrmsr(MSR_IA32_MCx_STATUS(b), 0);
    }

    if (!(mcg_status & MCG_STATUS_RIPV)) fatal = true;

    /* Clearing MCIP is what allows a *later* machine check to be delivered as
     * an exception instead of shutting the processor down. Do it even on the
     * fatal path: the panic printer is more useful than a silent reset. */
    if (g_cpu_info.has_mca) wrmsr(MSR_IA32_MCG_STATUS, 0);

    return !fatal;
}

void mce_poll(void)
{
    if (!g_mce_enabled || s_banks == 0) return;

    for (u32 b = 0; b < s_banks; b++) {
        u64 status = rdmsr(MSR_IA32_MCx_STATUS(b));
        if (!(status & MCI_STATUS_VAL)) continue;

        /* Uncorrected records found by polling belong to an error that was
         * never signalled — log them, but they are not ours to act on. */
        u64 addr = (status & MCI_STATUS_ADDRV) ? rdmsr(MSR_IA32_MCx_ADDR(b)) : 0;
        u64 misc = (status & MCI_STATUS_MISCV) ? rdmsr(MSR_IA32_MCx_MISC(b)) : 0;

        mce_print(b, status, addr, misc, !(status & MCI_STATUS_UC));
        mce_log(b, status, addr, misc, 0, 0, !(status & MCI_STATUS_UC));
        wrmsr(MSR_IA32_MCx_STATUS(b), 0);
    }
}

size_t mce_format(char *buf, size_t max)
{
    size_t off = 0;
    u32 total = __atomic_load_n(&s_log_total, __ATOMIC_RELAXED);

    off += scnprintf(buf + off, max > off ? max - off : 0,
                     "status      : %s\n"
                     "banks       : %u\n"
                     "mcg_cap     : 0x%016llx\n"
                     "records     : %u\n",
                     g_mce_enabled ? "enabled" : "unavailable",
                     s_banks, (unsigned long long)g_cpu_info.mcg_cap, total);

    u32 shown = total < MCE_LOG_ENTRIES ? total : MCE_LOG_ENTRIES;
    u32 start = (total >= MCE_LOG_ENTRIES) ? (total - MCE_LOG_ENTRIES) : 0;

    for (u32 i = 0; i < shown; i++) {
        const mce_record_t *e = &s_log[(start + i) % MCE_LOG_ENTRIES];
        if (!__atomic_load_n(&e->valid, __ATOMIC_ACQUIRE)) continue;
        off += scnprintf(buf + off, max > off ? max - off : 0,
                         "\nbank        : %u\n"
                         "severity    : %s\n"
                         "type        : %s\n"
                         "status      : 0x%016llx\n"
                         "addr        : 0x%016llx\n"
                         "misc        : 0x%016llx\n"
                         "mcgstatus   : 0x%016llx\n"
                         "rip         : 0x%016llx\n",
                         e->bank, e->corrected ? "corrected" : "uncorrected",
                         mce_error_type(e->status),
                         (unsigned long long)e->status,
                         (unsigned long long)e->addr,
                         (unsigned long long)e->misc,
                         (unsigned long long)e->mcg_status,
                         (unsigned long long)e->rip);
    }
    return off;
}
