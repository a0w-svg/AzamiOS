/* ============================================================================
 * AzamiOS — Kernel Image Self-Protection (W^X)
 * File: arch/x86_64/mm/kprotect.c
 *
 * See kprotect.h for the policy this enforces and why the HHDM alias is half
 * the point. This file is policy only: every page-table edit goes through
 * vmm_protect_range(), which knows how to rewrite a 1 GiB entry in place
 * rather than shattering it into a quarter of a million PTEs.
 * ============================================================================ */

#include "kprotect.h"
#include "vmm.h"
#include "tlb.h"
#include "../../../include/azami/defs.h"
#include "../../../drivers/char/console.h"
#include "../cpu/msr.h"

extern int scnprintf(char *buf, size_t size, const char *fmt, ...);

/* Section boundaries from scripts/kernel.ld. Declared as arrays so their
 * *addresses* are the values — a linker symbol has no storage to load from. */
extern u8 _text_start[], _text_end[];
extern u8 _rodata_start[], _rodata_end[];
extern u8 __extable_start[], __extable_end[];
extern u8 _ro_after_init_start[], _ro_after_init_end[];
extern u8 _data_start[], _data_end[];
extern u8 _bss_start[], _bss_end[];
extern u8 _kernel_end[];

/* ── What the seal did, kept for the report ──────────────────────────────── */

typedef struct {
    const char *name;
    u64         va_start;
    u64         va_end;
    u64         before;      /* leaf flags sampled before the change        */
    u64         after;       /* leaf flags sampled after                    */
    s64         changed;     /* entries vmm_protect_range() actually edited */
} kprot_region_t;

#define KPROT_MAX_REGIONS 8
static kprot_region_t s_regions[KPROT_MAX_REGIONS];
static u32  s_region_count   = 0;
static s64  s_hhdm_nx_edits  = -1;   /* -1 = not attempted */
static s64  s_hhdm_ro_edits  = -1;
static u64  s_edit_cycles    = 0;   /* page-table work, excluding the flush */
static u64  s_flush_cycles   = 0;   /* the one cross-CPU shootdown          */
static u64  s_hhdm_ro_bytes  = 0;
static bool s_sealed         = false;

bool kprotect_is_sealed(void) { return s_sealed; }

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static void record(const char *name, u64 start, u64 end, u64 set, u64 clear)
{
    if (s_region_count >= KPROT_MAX_REGIONS) return;
    kprot_region_t *r = &s_regions[s_region_count++];

    r->name     = name;
    r->va_start = ALIGN_DOWN(start, PAGE_SIZE);
    r->va_end   = ALIGN_UP(end, PAGE_SIZE);
    r->before   = vmm_query_flags(0, r->va_start);
    r->changed  = vmm_protect_range(0, r->va_start, r->va_end, set, clear,
                                    VMM_PROT_SPLIT | VMM_PROT_NOFLUSH);
    r->after    = vmm_query_flags(0, r->va_start);
}

/* Render one leaf's flag word the way a human reads a permission triple. */
static const char *perm_str(u64 flags, char out[8])
{
    if (!(flags & VMM_F_PRESENT)) { out[0] = '-'; out[1] = '-'; out[2] = '-'; out[3] = 0; return out; }
    out[0] = 'r';
    out[1] = (flags & VMM_F_WRITE) ? 'w' : '-';
    out[2] = (flags & VMM_F_NX)    ? '-' : 'x';
    out[3] = 0;
    return out;
}

/*
 * Apply @set/@clear to the HHDM aliases of a kernel-image range.
 *
 * The kernel image is contiguous in virtual address space but nothing
 * promises its frames are contiguous in physical memory, so this translates
 * page by page and coalesces maximal physical runs, then protects each run in
 * one call. In practice Limine loads the image as a single run and this makes
 * exactly one call; the loop exists so a fragmented load degrades into more
 * calls rather than into a wrong answer.
 *
 * Returns the number of entries edited, or -1 if any call failed.
 */
static s64 protect_hhdm_alias(u64 va_start, u64 va_end, u64 set, u64 clear,
                              u64 *bytes_out)
{
    va_start = ALIGN_DOWN(va_start, PAGE_SIZE);
    va_end   = ALIGN_UP(va_end, PAGE_SIZE);

    s64 total = 0;
    u64 run_phys = 0, run_len = 0;

    for (u64 va = va_start; va <= va_end; va += PAGE_SIZE) {
        phys_addr_t p = (va < va_end) ? vmm_translate(0, va) : 0;

        /* Extend the current run when this page continues it. */
        if (run_len && p == run_phys + run_len) { run_len += PAGE_SIZE; continue; }

        if (run_len) {
            s64 n = vmm_protect_range(0, (virt_addr_t)PHYS_TO_VIRT(run_phys),
                                      (virt_addr_t)PHYS_TO_VIRT(run_phys + run_len),
                                      set, clear, VMM_PROT_SPLIT | VMM_PROT_NOFLUSH);
            if (n < 0) return -1;
            total += n;
            if (bytes_out) *bytes_out += run_len;
        }
        run_phys = p;
        run_len  = p ? PAGE_SIZE : 0;
    }
    return total;
}

/* ── The seal ────────────────────────────────────────────────────────────── */

void kprotect_seal(void)
{
    if (s_sealed) return;
    u64 t_start = rdtsc();

    /* Read-only regions get NX as well as losing WRITE: constant data is not
     * code, and the two together are what makes a gadget hunt in .rodata
     * pointless rather than merely inconvenient. .text is the one region that
     * keeps its execute permission, and the one that must lose WRITE. */
    const u64 RO_NX_SET   = VMM_F_PRESENT | VMM_F_NX;
    const u64 RO_NX_CLEAR = VMM_F_WRITE;

    record(".text",           (u64)_text_start,          (u64)_text_end,
           VMM_F_PRESENT,                 VMM_F_WRITE | VMM_F_NX);
    record(".rodata",         (u64)_rodata_start,        (u64)_rodata_end,
           RO_NX_SET,                     RO_NX_CLEAR);
    record(".extable",        (u64)__extable_start,      (u64)__extable_end,
           RO_NX_SET,                     RO_NX_CLEAR);
    record(".ro_after_init",  (u64)_ro_after_init_start, (u64)_ro_after_init_end,
           RO_NX_SET,                     RO_NX_CLEAR);
    /* .data and .bss are adjacent and identically treated; one range spares a
     * second global shootdown. */
    record(".data+.bss",      (u64)_data_start,          (u64)_bss_end,
           VMM_F_PRESENT | VMM_F_WRITE | VMM_F_NX, 0);

    /*
     * The direct map. Every frame the PMM will ever hand out is visible here,
     * so leaving it executable means every kernel heap object is a potential
     * landing pad for a corrupted function pointer — the attacker does not
     * even need to find a mapping, they need only get their bytes into the
     * heap and jump to PHYS_TO_VIRT of wherever they landed. NX over the
     * whole window closes that in one stroke.
     *
     * allow_split = false is deliberate. The HHDM is mapped with 1 GiB and
     * 2 MiB pages, and that TLB reach is worth real throughput on every
     * kernel memory access; splitting it to express a byte-exact boundary we
     * do not need would pay for this mitigation with a permanent slowdown.
     * Nothing in the window wants to be executable, so whole-page granularity
     * is exactly right.
     */
    if (g_hhdm_phys_top) {
        s_hhdm_nx_edits = vmm_protect_range(0, HHDM_BASE,
                                            HHDM_BASE + g_hhdm_phys_top,
                                            VMM_F_NX, 0, VMM_PROT_NOFLUSH);
    }

    /*
     * ...and the other half of the job. The kernel image's frames are inside
     * that window too, so .text, .rodata and the freshly sealed
     * __ro_after_init table are all still writable at their HHDM address
     * however read-only they now are at -2 GB. Dropping WRITE from the alias
     * is what turns the records above from bookkeeping into enforcement.
     *
     * .data and .bss are deliberately not included: they are writable by
     * design, and the alias being writable too costs nothing.
     */
    s64 a = protect_hhdm_alias((u64)_text_start, (u64)_text_end,
                               VMM_F_NX, VMM_F_WRITE, &s_hhdm_ro_bytes);
    s64 b = protect_hhdm_alias((u64)_rodata_start, (u64)__extable_end,
                               VMM_F_NX, VMM_F_WRITE, &s_hhdm_ro_bytes);
    s64 c = protect_hhdm_alias((u64)_ro_after_init_start, (u64)_ro_after_init_end,
                               VMM_F_NX, VMM_F_WRITE, &s_hhdm_ro_bytes);
    s_hhdm_ro_edits = (a < 0 || b < 0 || c < 0) ? -1 : a + b + c;

    /*
     * One broadcast for the whole seal. Every edit above ran with
     * VMM_PROT_NOFLUSH, so none of them has been published yet — which is
     * fine, because nothing between them depends on the new permissions, and
     * doing it this way turns nine global shootdowns into one. Each broadcast
     * makes all three other cores stop and acknowledge, and any of them
     * spinning for g_vmm_lock behind this walk cannot answer until the walk
     * lets go; run nine of those back to back during boot and the shootdown
     * wait starts timing out and logging "[TLB] shootdown to CPUn stuck".
     */
    u64 t_edits = rdtsc();
    s_edit_cycles = t_edits - t_start;
    vmm_protect_flush();
    s_flush_cycles = rdtsc() - t_edits;

    s_sealed = true;

    /* static, not on the stack: the report is a few hundred bytes and kernel
     * stacks here are one page. */
    static char report[1024];
    kprotect_format(report, sizeof report);
    kprintf("%s", report);
}

/* ── Reporting ───────────────────────────────────────────────────────────── */

size_t kprotect_format(char *buf, size_t max)
{
    size_t off = 0;
    #define P(...) off += scnprintf(buf + off, max > off ? max - off : 0, __VA_ARGS__)

    if (!s_sealed) {
        P("[KPROT] not sealed\n");
        return off;
    }

    char was[8], now[8];
    s64 total = 0;
    for (u32 i = 0; i < s_region_count; i++) total += s_regions[i].changed;

    P("[KPROT] kernel image W^X sealed: %lld image entries, "
      "HHDM NX %lld entries, HHDM read-only alias %llu KiB "
      "[%llu Mcyc edits + %llu Mcyc shootdown]\n",
      (long long)total,
      (long long)(s_hhdm_nx_edits < 0 ? 0 : s_hhdm_nx_edits),
      (unsigned long long)(s_hhdm_ro_bytes >> 10),
      (unsigned long long)(s_edit_cycles / 1000000),
      (unsigned long long)(s_flush_cycles / 1000000));

    for (u32 i = 0; i < s_region_count; i++) {
        kprot_region_t *r = &s_regions[i];
        P("[KPROT]   %-16s %016llx-%016llx %3llu KiB  %s -> %s%s\n",
          r->name,
          (unsigned long long)r->va_start, (unsigned long long)r->va_end,
          (unsigned long long)((r->va_end - r->va_start) >> 10),
          perm_str(r->before, was), perm_str(r->after, now),
          r->changed == 0 ? " (already correct)" : "");
    }

    if (s_hhdm_nx_edits < 0) P("[KPROT]   HHDM NX: FAILED\n");
    if (s_hhdm_ro_edits < 0) P("[KPROT]   HHDM read-only alias: FAILED\n");

    /*
     * Read the enforcement back out of the hardware rather than reporting
     * what we asked for. The alias is the half that is easy to get wrong and
     * impossible to notice: the primary mapping can look perfect while the
     * same bytes stay writable and executable one address away, and nothing
     * in the system would ever tell you.
     */
    struct { const char *what; u64 va; } probe[] = {
        { ".text via HHDM",           (u64)(uintptr_t)PHYS_TO_VIRT(vmm_translate(0, (virt_addr_t)_text_start)) },
        { ".ro_after_init via HHDM",  (u64)(uintptr_t)PHYS_TO_VIRT(vmm_translate(0, (virt_addr_t)_ro_after_init_start)) },
        { "kernel heap via HHDM",     (u64)(uintptr_t)PHYS_TO_VIRT(vmm_translate(0, (virt_addr_t)_bss_start)) },
    };
    for (u32 i = 0; i < sizeof probe / sizeof probe[0]; i++) {
        u64 f = vmm_query_flags(0, (virt_addr_t)probe[i].va);
        P("[KPROT]   alias check: %-24s %016llx  %s\n",
          probe[i].what, (unsigned long long)probe[i].va, perm_str(f, was));
    }

    #undef P
    return off;
}
