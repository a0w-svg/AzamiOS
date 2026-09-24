/* ============================================================================
 * AzamiOS — kexec: load and jump into a fresh copy of the same kernel build
 * File: kernel/kexec.c
 *
 * What real kexec is, scoped down to what this kernel can actually support:
 *
 * Real Linux kexec(8) loads an arbitrary second kernel image and tears down
 * the first to jump straight into it, skipping a real hardware reset. Full
 * generality — an arbitrary bzImage, 16-bit real-mode trampolines, a second
 * initrd — makes no sense for a Limine-booted kernel that has none of that
 * boot machinery. What *is* both tractable and genuinely useful (it is what
 * real kexec is used for in production far more often than people assume —
 * a fast reboot into the same or a newer build) is: kexec_file_load(2) loads
 * another ELF build of *this same kernel* (same linker layout, same
 * KERNEL_BASE), and reboot(2, LINUX_REBOOT_CMD_KEXEC) actually jumps into
 * it instead of resetting the hardware.
 *
 * ── Why this is safe without rebuilding page tables ─────────────────────
 * arch/x86_64/mm/vmm.c's vmm_init() does not construct a page table at all —
 * it trusts whatever CR3 already holds when it runs (Limine built it) and
 * just records it. That means the kernel's page table is one long-lived
 * object for the life of the system, and kexec does not need a second one:
 * the intermediate PML4/PDPT/PD/PT levels covering the kernel's own
 * [_text_start, _kernel_end) virtual range already exist (the running
 * kernel is mapped there right now). All a kexec has to do is overwrite the
 * *leaf* PT entries in that range to point at freshly loaded physical
 * frames, then jump to the new entry point. No new tables, no new address
 * space, no touching user mappings at all.
 *
 * The HHDM (0xFFFF800000000000 + offset) is never touched by any of this —
 * only the KERNEL_BASE (-2 GB) mapping is rewritten — which is what makes it
 * safe to do all the preparation (ELF parsing, physical allocation, content
 * copying, even building the exact list of PTE writes to make) as completely
 * ordinary C code, right up until the moment the writes themselves happen.
 *
 * ── The actually dangerous part ─────────────────────────────────────────
 * Once even one leaf PTE in the running kernel's own virtual range has been
 * rewritten, an instruction fetch from that VA reads the *new* kernel's
 * bytes. Doing the rewrite from code that itself lives in that range would
 * mean overwriting the mapping out from under the very fetch stream
 * executing it. So the rewrite loop, the TLB flush and the final jump all
 * run from a tiny hand-written asm blob (arch/x86_64/cpu/kexec_trampoline.
 * asm) whose *compiled bytes* are copied into a scratch page mapped at a
 * throwaway virtual address, well away from both this kernel's own image
 * and the HHDM — see kexec_execute() below.
 *
 * The other cores are a related hazard: they share this exact page table
 * (there is one kernel PML4 system-wide, not one per core), so the instant
 * the rewrite starts, every core's view of KERNEL_BASE changes together.
 * Every other online CPU is therefore parked (cli;hlt, via a dedicated IPI —
 * see KEXEC_PARK_VECTOR in kernel/kexec.h and its handler in arch/x86_64/
 * cpu/idt.c) *before* a single PTE is touched, and kexec_execute() confirms
 * every one of them has actually parked (with a timeout) before proceeding.
 *
 * ── Scope cuts, stated plainly ───────────────────────────────────────────
 *   - BSP-only after the jump. Parked APs have no way back — Limine's SMP
 *     wake handshake is a one-time protocol from the very first real boot,
 *     not something that can be replayed. The new kernel instance is handed
 *     a forged "Limine found no SMP" response so its own smp_init() takes
 *     the ordinary single-CPU fallback path instead of hanging waiting for
 *     cores that will never answer. A from-scratch second AP-wake protocol
 *     is real, separate complexity and is explicitly out of scope here.
 *   - No initrd. This kernel has no initrd boot mechanism at all — root is
 *     always a real disk partition — so a non-trivial initrd_fd is a real
 *     -EINVAL, not something silently ignored.
 *   - Only a compatible build. kexec_load() checks every PT_LOAD vaddr
 *     against this kernel's own KERNEL_BASE window and kexec_execute()
 *     checks that the Limine request objects it needs to patch actually
 *     exist at the expected offsets inside the staged image. Anything else
 *     is rejected with a real errno, never silently mis-staged.
 * ============================================================================ */

#define DEBUG 1
#include "../include/azami/debug.h"
#include "kexec.h"
#include "../fs/vfs.h"
#include "sched/elf.h"
#include "mm/pmm.h"
#include "mm/kmalloc.h"
#include "lib/string.h"
#include "../arch/x86_64/mm/vmm.h"
#include "../arch/x86_64/cpu/spinlock.h"
#include "../arch/x86_64/cpu/smp.h"
#include "../arch/x86_64/cpu/msr.h"
#include "../arch/x86_64/cpu/hwaccel.h"
#include "../arch/x86_64/boot/limine_req.h"
#include "../drivers/char/console.h"

/* This kernel's own link base — scripts/kernel.ld's KERNEL_BASE. A staged
 * image's PT_LOAD segments must fall inside [KERNEL_IMAGE_BASE,
 * KERNEL_IMAGE_BASE + KEXEC_MAX_IMAGE_SIZE) or it is not "this same kernel
 * build" and is rejected. */
#define KERNEL_IMAGE_BASE       0xFFFFFFFF80000000ULL

/* Generous but bounded: protects against a garbage/huge file trying to walk
 * the allocator out of physical memory. The real running kernel is currently
 * ~40 MB (see build/kernel.elf); 256 MB leaves enormous headroom for growth
 * while still being a real, enforced bound rather than "however much RAM the
 * machine happens to have". */
#define KEXEC_MAX_IMAGE_SIZE    (256UL * 1024 * 1024)
#define KEXEC_MAX_PAGES         (KEXEC_MAX_IMAGE_SIZE / PAGE_SIZE)

#define KEXEC_MAX_CMDLINE       1024

/* Scratch VA for the copied trampoline: 2 GB below KERNEL_BASE (still
 * canonical kernel-half address space) — nowhere near this kernel's own
 * [_text_start,_kernel_end) image (which the PTE rewrite targets) and
 * nowhere near the HHDM (which only ever spans installed RAM, a few GB at
 * most on any machine this boots on). vmm_map() creates whatever
 * intermediate page-table levels this needs the ordinary way; nothing about
 * this address is otherwise special. */
#define KEXEC_TRAMPOLINE_VA     0xFFFFFFFE00000000ULL

/* Spin-wait bound for AP parking. Generous: on an idle core (which is what
 * every AP but the one calling kexec_execute() should be) the IPI is
 * serviced within a handful of iterations. This only exists to turn "an AP
 * wedged with interrupts disabled somewhere" into a reported failure
 * instead of a silent hang. */
#define KEXEC_PARK_TIMEOUT_ITERS  200000000ULL

/* Generous upper bound on how many live kernel-half page-table pages
 * kexec_collect_live_table_pages() could ever find on a real system — see
 * that function's doc comment for why they must be reserved. In practice
 * this kernel's own footprint is a handful of top-level entries deep (a few
 * hundred table pages at most); this is sized for enormous headroom rather
 * than tuned tightly, since the only cost of an oversized cap is one
 * kmalloc() of a few hundred KB during a deliberate reboot. */
#define KEXEC_MAX_TABLE_PAGES     65536

/* Upper bound on the number of individually-kmalloc'd "persistent forged
 * state" objects kexec_execute() ever creates (one framebuffer struct/array/
 * response, one each for HHDM/RSDP/kernel-address/boot-time, three for the
 * memory map itself). Generous headroom over the actual count (currently
 * ~10) costs nothing — see kexec_reserve_ptr(). */
#define KEXEC_MAX_EXTRA_RESERVE   32

/* ── The staged image ─────────────────────────────────────────────────────
 * One physical frame per staged 4 KiB page of the candidate kernel's own
 * virtual footprint, plus the flags that page should carry once mapped.
 * Populated by kexec_load(), consumed (read-only) by kexec_execute(). */
typedef struct kexec_page {
    virt_addr_t vaddr;      /* target VA, page aligned, inside KERNEL_IMAGE_BASE..+MAX */
    phys_addr_t phys;       /* freshly allocated frame already holding this page's content */
    u64         pte_flags;  /* VMM_F_* flags (PRESENT|GLOBAL[|WRITE][|NX]) for the new PTE */
} kexec_page_t;

typedef struct {
    bool          loaded;
    u64           entry;              /* ELF e_entry: VA of the new az_boot_entry */
    kexec_page_t *pages;              /* sorted ascending by vaddr (PT_LOAD file order) */
    size_t        page_count;
    char          cmdline[KEXEC_MAX_CMDLINE];
    size_t        cmdline_len;
    spinlock_t    lock;
} kexec_image_t;

static kexec_image_t g_kexec_image = { .lock = SPINLOCK_INIT };

/* Bumped by every AP's park handler (arch/x86_64/cpu/idt.c) as it parks.
 * See KEXEC_PARK_VECTOR's doc comment in kernel/kexec.h. */
volatile u32 g_kexec_park_count = 0;

/* Raw bytes of the trampoline, linked into this kernel's own .text like any
 * other .asm file — see arch/x86_64/cpu/kexec_trampoline.asm for why it can
 * never be *executed* from there. */
extern const u8 kexec_trampoline[];
extern const u8 kexec_trampoline_end[];

/* One PTE write the trampoline performs. @pte_ptr is an HHDM pointer to the
 * live leaf PTE slot (stable across the whole kexec — see the file banner),
 * @new_value is the complete replacement PTE (new phys | flags). Layout must
 * match the trampoline asm's assumptions exactly (two u64s, in this order). */
typedef struct {
    u64 *pte_ptr;
    u64  new_value;
} kexec_poke_t;

/* ── Free a previously staged image. Caller holds g_kexec_image.lock. ────── */
static void kexec_free_staged_locked(void)
{
    if (g_kexec_image.pages) {
        for (size_t i = 0; i < g_kexec_image.page_count; i++) {
            pmm_free_page(g_kexec_image.pages[i].phys);
        }
        kfree(g_kexec_image.pages);
    }
    g_kexec_image.pages = NULL;
    g_kexec_image.page_count = 0;
    g_kexec_image.entry = 0;
    g_kexec_image.loaded = false;
}

/* ============================================================================
 * kexec_load() — validate a candidate kernel ELF and stage it in memory.
 * ============================================================================ */
int kexec_load(struct file *kfile, const char *cmdline, size_t cmdline_len,
               unsigned long flags)
{
    if (!kfile) return -(int)EBADF;

    /* No KEXEC_FILE_* behaviour is implemented (no on-crash slot, no
     * unload-only mode, no debug dump). Claiming to honour a flag that
     * silently does nothing would be exactly the dishonesty this whole
     * feature replaces, so any nonzero flags is a real -EINVAL. */
    if (flags != 0) return -(int)EINVAL;

    struct stat st;
    if (vfs_fstat(kfile, &st) != 0) return -(int)ENOEXEC;
    if (st.st_size <= 0 || (u64)st.st_size > KEXEC_MAX_IMAGE_SIZE) return -(int)ENOEXEC;

    elf64_ehdr_t ehdr;
    kfile->f_pos = 0;
    if (vfs_read(kfile, &ehdr, sizeof(ehdr)) != (s64)sizeof(ehdr)) return -(int)ENOEXEC;

    if (ehdr.e_ident_magic != ELF_MAGIC ||
        ehdr.e_ident_class != ELFCLASS64 ||
        ehdr.e_ident_data  != ELFDATA2LSB ||
        ehdr.e_machine     != EM_X86_64 ||
        ehdr.e_type        != ET_EXEC ||
        ehdr.e_phnum       == 0 ||
        ehdr.e_phentsize   != sizeof(elf64_phdr_t)) {
        return -(int)ENOEXEC;
    }

    /* The entry point has to land inside this kernel's own link window, or
     * this is not "the same build" (or a compatible one) at all. */
    if (ehdr.e_entry < KERNEL_IMAGE_BASE ||
        ehdr.e_entry >= KERNEL_IMAGE_BASE + KEXEC_MAX_IMAGE_SIZE) {
        return -(int)ENOEXEC;
    }

    /* ---- Pass 1: validate every PT_LOAD and total the page footprint ---- */
    size_t total_pages = 0;
    for (u16 i = 0; i < ehdr.e_phnum; i++) {
        elf64_phdr_t phdr;
        kfile->f_pos = ehdr.e_phoff + (u64)i * ehdr.e_phentsize;
        if (vfs_read(kfile, &phdr, sizeof(phdr)) != (s64)sizeof(phdr)) return -(int)ENOEXEC;
        if (phdr.p_type != PT_LOAD || phdr.p_memsz == 0) continue;

        if (phdr.p_filesz > phdr.p_memsz) return -(int)ENOEXEC;
        if (phdr.p_vaddr < KERNEL_IMAGE_BASE ||
            phdr.p_vaddr + phdr.p_memsz < phdr.p_vaddr ||   /* overflow */
            phdr.p_vaddr + phdr.p_memsz > KERNEL_IMAGE_BASE + KEXEC_MAX_IMAGE_SIZE) {
            return -(int)ENOEXEC;
        }

        u64 start = ALIGN_DOWN(phdr.p_vaddr, PAGE_SIZE);
        u64 end   = ALIGN_UP(phdr.p_vaddr + phdr.p_memsz, PAGE_SIZE);
        total_pages += (end - start) / PAGE_SIZE;
    }
    if (total_pages == 0 || total_pages > KEXEC_MAX_PAGES) return -(int)ENOEXEC;

    kexec_page_t *pages = kmalloc(total_pages * sizeof(kexec_page_t));
    if (!pages) return -(int)ENOMEM;

    /* ---- Pass 2: allocate + populate every page ---- */
    size_t n = 0;
    int rc = 0;
    for (u16 i = 0; i < ehdr.e_phnum && rc == 0; i++) {
        elf64_phdr_t phdr;
        kfile->f_pos = ehdr.e_phoff + (u64)i * ehdr.e_phentsize;
        if (vfs_read(kfile, &phdr, sizeof(phdr)) != (s64)sizeof(phdr)) { rc = -(int)ENOEXEC; break; }
        if (phdr.p_type != PT_LOAD || phdr.p_memsz == 0) continue;

        u64 vmm_flags = VMM_F_PRESENT | VMM_F_GLOBAL;
        if (phdr.p_flags & PF_W) vmm_flags |= VMM_F_WRITE;
        if (!(phdr.p_flags & PF_X)) vmm_flags |= VMM_F_NX;

        u64 seg_vaddr   = phdr.p_vaddr;
        u64 start_vaddr = ALIGN_DOWN(seg_vaddr, PAGE_SIZE);
        u64 end_vaddr   = ALIGN_UP(seg_vaddr + phdr.p_memsz, PAGE_SIZE);

        for (u64 vaddr = start_vaddr; vaddr < end_vaddr; vaddr += PAGE_SIZE) {
            phys_addr_t phys = pmm_alloc_page();
            if (!phys) { rc = -(int)ENOMEM; break; }
            void *dst = (void *)PHYS_TO_VIRT(phys);

            /* Same streaming copy/zero pattern as the ordinary ELF loader
             * (kernel/sched/elf.c: load_elf_segments()) — overlap this
             * page against the segment's file-backed range, copy what's
             * there, zero the rest (covers both intra-page padding and
             * pure-BSS tail pages, e.g. .bss). */
            u64 page_end       = vaddr + PAGE_SIZE;
            u64 file_data_end  = seg_vaddr + phdr.p_filesz;
            u64 file_start     = (vaddr < seg_vaddr) ? seg_vaddr : vaddr;
            u64 file_end       = (page_end < file_data_end) ? page_end : file_data_end;

            if (file_end > file_start) {
                u64 page_off = file_start - vaddr;
                size_t read_bytes = (size_t)(file_end - file_start);
                if (page_off > 0) memset(dst, 0, (size_t)page_off);
                kfile->f_pos = phdr.p_offset + (file_start - seg_vaddr);
                s64 nread = vfs_read(kfile, (char *)dst + page_off, read_bytes);
                if (nread < (s64)read_bytes) { pmm_free_page(phys); rc = -(int)EIO; break; }
                if (page_off + read_bytes < PAGE_SIZE)
                    memset((char *)dst + page_off + read_bytes, 0,
                           PAGE_SIZE - (page_off + read_bytes));
            } else {
                hw_clear_page(dst);
            }

            pages[n].vaddr = vaddr;
            pages[n].phys = phys;
            pages[n].pte_flags = vmm_flags;
            n++;
        }
    }

    if (rc != 0) {
        for (size_t i = 0; i < n; i++) pmm_free_page(pages[i].phys);
        kfree(pages);
        return rc;
    }

    /* ---- Commit: swap into the global staged image ---- */
    irqflags_t irqf = spinlock_lock_irqsave(&g_kexec_image.lock);
    kexec_free_staged_locked();   /* replace, don't leak, any prior staged image */
    g_kexec_image.pages = pages;
    g_kexec_image.page_count = n;
    g_kexec_image.entry = ehdr.e_entry;
    if (cmdline && cmdline_len > 0) {
        size_t copy_len = cmdline_len < KEXEC_MAX_CMDLINE - 1 ? cmdline_len : KEXEC_MAX_CMDLINE - 1;
        memcpy(g_kexec_image.cmdline, cmdline, copy_len);
        g_kexec_image.cmdline[copy_len] = '\0';
        g_kexec_image.cmdline_len = copy_len;
    } else {
        g_kexec_image.cmdline[0] = '\0';
        g_kexec_image.cmdline_len = 0;
    }
    g_kexec_image.loaded = true;
    spinlock_unlock_irqrestore(&g_kexec_image.lock, irqf);

    kprintf("[KEXEC] Staged kernel image: entry=0x%016llx, %zu pages (%llu KB)\n",
            (unsigned long long)ehdr.e_entry, n,
            (unsigned long long)((u64)n * PAGE_SIZE / 1024));
    return 0;
}

/* ── Find, in the *currently live* page table, the leaf PTE slot backing a
 * kernel virtual address. Mirrors vmm.c's private phys_to_table() walk using
 * only the public index macros / masks in vmm.h — vmm.c does not export a
 * "give me the slot" primitive, and this is read-only until the trampoline
 * itself writes through the returned pointer. Returns NULL if any level is
 * not present or is a huge page (the kernel image is never mapped through
 * huge pages, so that would indicate an incompatible/corrupt page table). */
static u64 *kexec_find_pte_slot(virt_addr_t va)
{
    u64 cr3 = read_cr3() & VMM_PHYS_MASK;
    u64 *pml4 = (u64 *)PHYS_TO_VIRT(cr3);
    if (!(pml4[VMM_PML4_IDX(va)] & VMM_F_PRESENT)) return NULL;

    u64 *pdpt = (u64 *)PHYS_TO_VIRT(pml4[VMM_PML4_IDX(va)] & VMM_PHYS_MASK);
    u64 pdpte = pdpt[VMM_PDPT_IDX(va)];
    if (!(pdpte & VMM_F_PRESENT) || (pdpte & VMM_F_HUGE)) return NULL;

    u64 *pd = (u64 *)PHYS_TO_VIRT(pdpte & VMM_PHYS_MASK);
    u64 pde = pd[VMM_PD_IDX(va)];
    if (!(pde & VMM_F_PRESENT) || (pde & VMM_F_HUGE)) return NULL;

    u64 *pt = (u64 *)PHYS_TO_VIRT(pde & VMM_PHYS_MASK);
    return &pt[VMM_PT_IDX(va)];
}

/* ── Collect every physical page that makes up the *intermediate levels* of
 * the live kernel-half page table: the PML4 page itself, plus every present
 * PDPT/PD/PT page reachable by walking PML4 entries 256..511 (the kernel
 * half — vmm_create_space()/vmm_clone_space() already document that these
 * are shared, never copied, across every address space in this kernel).
 *
 * This is NOT redundant with the "new image" / "AP stacks" / "old kernel
 * range" reservations elsewhere in kexec_execute(): those cover data the
 * kexec itself introduces or leaves dangling, but this page table is a
 * *pre-existing, still-in-use* object neither kexec_load() nor
 * kexec_execute() ever allocates — per the file banner, vmm_init() in the
 * new kernel instance does not build a new one, it just re-reads CR3 and
 * keeps using this exact table for the rest of that kernel's life. Most of
 * these table pages were themselves allocated out of ordinary USABLE memory
 * at various points during *this* boot (e.g. vmm_init()'s ACPI/MMIO 4 KiB
 * fix-up calls vmm_map(), which calls alloc_table() → pmm_alloc_page() the
 * same as any other allocation) — Limine's original memmap snapshot has no
 * way to know that happened, so nothing else in this file's memmap
 * carve-out would ever exclude them. Missing this reservation means the new
 * kernel's own allocator can and will eventually hand one of these frames
 * to something else entirely (a kernel stack, a kmalloc block, ...), which
 * silently corrupts the very page table both the allocation and every other
 * live translation on the system depend on — this was, in fact, exactly the
 * bug that produced the first real kexec test failure during development
 * (a `#GP` deep inside the new kernel's own pmm free-list, from a table page
 * having been hijacked as an ordinary stack allocation moments earlier).
 *
 * Bounded by @cap: writes at most @cap entries, but always returns the
 * *true* total count found, even past @cap — so a caller can tell truncation
 * apart from "that's really all of them" by comparing the return value
 * against @cap, instead of a truncated call silently looking identical to a
 * complete one. In practice this kernel's own kernel-half footprint is a
 * handful of top-level entries deep — a few hundred table pages at most —
 * so @cap is sized with enormous headroom by the caller rather than tuned
 * tightly. */
static size_t kexec_collect_live_table_pages(phys_addr_t *out, size_t cap)
{
    size_t n = 0;
#define KEXEC_EMIT(phys) do { if (n < cap) out[n] = (phys); n++; } while (0)

    u64 cr3 = read_cr3() & VMM_PHYS_MASK;
    KEXEC_EMIT(cr3);   /* the PML4 page itself */

    u64 *pml4 = (u64 *)PHYS_TO_VIRT(cr3);
    for (int i4 = 256; i4 < 512; i4++) {
        if (!(pml4[i4] & VMM_F_PRESENT)) continue;
        phys_addr_t pdpt_phys = pml4[i4] & VMM_PHYS_MASK;
        KEXEC_EMIT(pdpt_phys);

        u64 *pdpt = (u64 *)PHYS_TO_VIRT(pdpt_phys);
        for (int i3 = 0; i3 < 512; i3++) {
            u64 pdpte = pdpt[i3];
            if (!(pdpte & VMM_F_PRESENT) || (pdpte & VMM_F_HUGE)) continue;
            phys_addr_t pd_phys = pdpte & VMM_PHYS_MASK;
            KEXEC_EMIT(pd_phys);

            u64 *pd = (u64 *)PHYS_TO_VIRT(pd_phys);
            for (int i2 = 0; i2 < 512; i2++) {
                u64 pde = pd[i2];
                if (!(pde & VMM_F_PRESENT) || (pde & VMM_F_HUGE)) continue;
                phys_addr_t pt_phys = pde & VMM_PHYS_MASK;
                KEXEC_EMIT(pt_phys);
                /* PT is the leaf table level — nothing further below it. */
            }
        }
    }
#undef KEXEC_EMIT
    return n;
}

/* ── Locate the byte, inside the *staged* image, corresponding to a runtime
 * virtual address in *this* (currently running) kernel. Because kexec_load()
 * already checked the staged image links at the same KERNEL_IMAGE_BASE with
 * the same page layout, a symbol's VA in the running kernel identifies the
 * matching page in the staged image directly — no need to parse the ELF's
 * symbol table at all. Returns NULL if the running kernel's VA is outside
 * every staged page (e.g. the images turn out not to be layout-compatible
 * after all), which the caller must treat as a real, reported failure. */
static void *kexec_new_image_ptr(const kexec_image_t *img, u64 runtime_va)
{
    u64 page_va = ALIGN_DOWN(runtime_va, PAGE_SIZE);
    for (size_t i = 0; i < img->page_count; i++) {
        if (img->pages[i].vaddr == page_va) {
            return (void *)((u8 *)PHYS_TO_VIRT(img->pages[i].phys) + (runtime_va & 0xFFFULL));
        }
    }
    return NULL;
}

/* Patch one Limine request object's .response field, inside the staged
 * image, to point at a freshly built response (or NULL for "not present").
 * @req_response_addr is &g_limine_..._req.response in the *running* kernel.
 * Returns false (and leaves *out_rc set) if the staged image has no page
 * backing that address — a real incompatible-image failure. */
static bool kexec_patch_response(const kexec_image_t *img, volatile void *req_response_addr,
                                  void *response, int *out_rc)
{
    void *slot = kexec_new_image_ptr(img, (u64)(uintptr_t)req_response_addr);
    if (!slot) {
        kprintf("[KEXEC] staged image is missing a page for a Limine request "
                "object at 0x%016llx — not layout-compatible, aborting\n",
                (unsigned long long)(uintptr_t)req_response_addr);
        *out_rc = -(int)ENOEXEC;
        return false;
    }
    *(void **)slot = response;
    return true;
}

/* ── Simple in-place quicksort over an array of physical addresses. No
 * qsort() is available in the freestanding kernel lib; recursion depth is
 * bounded by log2(count), trivial for a kernel stack even at tens of
 * thousands of entries. Used only to coalesce the staged image's (likely
 * mostly-contiguous, since they were all allocated back-to-back from an
 * otherwise-idle buddy allocator) physical pages into runs before building
 * the forged memory map. */
static void kexec_sort_phys(phys_addr_t *a, s64 lo, s64 hi)
{
    while (lo < hi) {
        phys_addr_t pivot = a[(size_t)((lo + hi) / 2)];
        s64 i = lo, j = hi;
        while (i <= j) {
            while (a[(size_t)i] < pivot) i++;
            while (a[(size_t)j] > pivot) j--;
            if (i <= j) {
                phys_addr_t t = a[(size_t)i]; a[(size_t)i] = a[(size_t)j]; a[(size_t)j] = t;
                i++; j--;
            }
        }
        /* Recurse into the smaller half, loop over the larger — bounds the
         * recursion depth to O(log n) even on adversarial input. */
        if (j - lo < hi - i) {
            kexec_sort_phys(a, lo, j);
            lo = i;
        } else {
            kexec_sort_phys(a, i, hi);
            hi = j;
        }
    }
}

/* One [base, base+len) physical range plus a Limine memmap entry type. Used
 * both for the base memmap copy and for the reservations carved out of it. */
typedef struct { phys_addr_t base, len; u64 type; } kexec_mm_range_t;

/* Subtract [cut_base, cut_base+cut_len) from every USABLE entry in work[0..
 * count), splitting an entry that straddles the cut into up to two
 * surviving USABLE entries. Non-USABLE entries are left alone (Limine
 * itself already reported them as unusable; nothing here needs to be
 * smarter than pmm_init(), which only ever frees LIMINE_MEMMAP_USABLE).
 * @cap bounds how many new (split-off) entries may be appended; a cut that
 * would exceed it drops the excess (logged) rather than corrupting memory —
 * callers size @cap generously enough that this never actually happens.
 * Returns the new entry count. */
static size_t kexec_carve(kexec_mm_range_t *work, size_t count, size_t cap,
                           phys_addr_t cut_base, phys_addr_t cut_len)
{
    if (cut_len == 0) return count;
    phys_addr_t cut_end = cut_base + cut_len;
    size_t n = count;

    for (size_t i = 0; i < count; i++) {
        if (work[i].type != LIMINE_MEMMAP_USABLE || work[i].len == 0) continue;
        phys_addr_t e_base = work[i].base;
        phys_addr_t e_end  = e_base + work[i].len;
        if (cut_end <= e_base || cut_base >= e_end) continue;   /* no overlap */

        phys_addr_t ov_start = cut_base > e_base ? cut_base : e_base;
        phys_addr_t ov_end   = cut_end  < e_end  ? cut_end  : e_end;
        bool has_before = ov_start > e_base;
        bool has_after  = ov_end   < e_end;

        if (has_before) {
            work[i].len = ov_start - e_base;             /* keep [e_base, ov_start) */
        } else if (has_after) {
            work[i].base = ov_end;
            work[i].len  = e_end - ov_end;                /* keep [ov_end, e_end) */
        } else {
            work[i].len = 0;                              /* entirely consumed */
        }

        if (has_before && has_after) {
            if (n >= cap) {
                kprintf("[KEXEC] WARNING: memmap carve ran out of room; "
                        "a free range will be under-reported (safe, just wastes memory)\n");
                continue;
            }
            work[n].base = ov_end;
            work[n].len  = e_end - ov_end;
            work[n].type = LIMINE_MEMMAP_USABLE;
            n++;
        }
    }
    return n;
}

/* Record that the page(s) backing a heap pointer must be reserved in the
 * forged memory map — see the doc comment on kexec_execute()'s
 * extra_reserve array for why. @size covers exactly the object at @ptr; the
 * call rounds out to whole pages and tolerates (indeed expects) the same
 * page being reserved more than once if several small objects share it —
 * kexec_carve() treats a redundant cut as a no-op, not an error. A NULL
 * @ptr (an absent optional response, e.g. no framebuffer) is silently
 * ignored. Drops the reservation (logged) rather than corrupting memory if
 * @cap is somehow exceeded — sized with headroom in the caller specifically
 * so this never happens in practice. */
static void kexec_reserve_ptr(kexec_mm_range_t *arr, size_t *n, size_t cap,
                               const void *ptr, size_t size)
{
    if (!ptr || size == 0) return;
    if (*n >= cap) {
        kprintf("[KEXEC] WARNING: extra_reserve capacity exceeded; a forged "
                "response object will not be protected from reuse\n");
        return;
    }
    phys_addr_t phys = VIRT_TO_PHYS((virt_addr_t)(uintptr_t)ptr);
    phys_addr_t start = ALIGN_DOWN(phys, PAGE_SIZE);
    phys_addr_t end   = ALIGN_UP(phys + size, PAGE_SIZE);
    arr[*n].base = start;
    arr[*n].len  = end - start;
    arr[*n].type = LIMINE_MEMMAP_RESERVED;
    (*n)++;
}

/* ============================================================================
 * kexec_execute() — the real thing. See the file banner and kexec.h for the
 * full design and the ordering guarantee: everything that can fail runs
 * before a single AP is parked; nothing after that point is allowed to fail.
 * ============================================================================ */
s64 kexec_execute(void)
{
    irqflags_t img_irqf = spinlock_lock_irqsave(&g_kexec_image.lock);
    if (!g_kexec_image.loaded) {
        spinlock_unlock_irqrestore(&g_kexec_image.lock, img_irqf);
        return -(s64)EINVAL;
    }
    /* Snapshot the fields kexec_execute() needs; the lock only protects
     * kexec_load() from racing a concurrent kexec_load(), not the long
     * (non-blocking, no-sleep) sequence below. A concurrent kexec_load()
     * swapping in a *different* image while this function is mid-flight
     * would be a genuine race, but sys_reboot_impl() is a one-shot syscall
     * per call and nothing else in this kernel calls kexec_execute(), so in
     * practice this is uncontended; the lock is held only long enough to
     * take a consistent snapshot. */
    kexec_image_t img = g_kexec_image;
    spinlock_unlock_irqrestore(&g_kexec_image.lock, img_irqf);

    int rc = 0;

    /* Every kmalloc() below whose result is patched into a Limine request's
     * .response field (or hangs off one, like the framebuffer's array/copy)
     * must stay intact for the *entire lifetime* of the new kernel instance,
     * not just through its early boot: az_boot_framebuffer() and friends
     * (arch/x86_64/boot/limine_req.h) are read throughout ordinary runtime
     * (drivers/video/fbdev.c, drivers/base/platform.c, drivers/input/
     * virtio_input.c, framebuffer ioctls in kernel/syscall/syscall.c — and,
     * if that instance ever kexecs again itself, this very function). Left
     * unreserved, this is ordinary USABLE memory as far as the new kernel's
     * allocator is concerned, and it *will* eventually be recycled for
     * something else — which is exactly the bug an earlier version of this
     * function had: a #GP deep inside az_boot_framebuffer() once the heap
     * page underneath a forged response got reused. Every such allocation
     * below is registered here and folded into the memmap reservation
     * alongside the staged image, the AP stacks and the live page table. */
    kexec_mm_range_t extra_reserve[KEXEC_MAX_EXTRA_RESERVE];
    size_t extra_reserve_n = 0;
#define RESERVE_PTR(ptr) \
    kexec_reserve_ptr(extra_reserve, &extra_reserve_n, KEXEC_MAX_EXTRA_RESERVE, \
                       (ptr), (ptr) ? sizeof(*(ptr)) : 0)

    /* ---- 1. Build the PTE poke list (fully constructed, not yet applied) ---- */
    kexec_poke_t *pokes = kmalloc(img.page_count * sizeof(kexec_poke_t));
    if (!pokes) return -(s64)ENOMEM;

    for (size_t i = 0; i < img.page_count; i++) {
        u64 *slot = kexec_find_pte_slot(img.pages[i].vaddr);
        if (!slot) {
            kprintf("[KEXEC] no live PTE for staged VA 0x%016llx — refusing to kexec\n",
                    (unsigned long long)img.pages[i].vaddr);
            kfree(pokes);
            return -(s64)ENXIO;
        }
        pokes[i].pte_ptr = slot;
        pokes[i].new_value = (img.pages[i].phys & VMM_PHYS_MASK) | img.pages[i].pte_flags;
    }

    /* ---- 2. Map the trampoline's copied bytes at a safe scratch VA ---- */
    phys_addr_t tramp_phys = pmm_alloc_page();
    if (!tramp_phys) { kfree(pokes); return -(s64)ENOMEM; }

    size_t tramp_len = (size_t)(kexec_trampoline_end - kexec_trampoline);
    if (tramp_len > PAGE_SIZE) {
        /* Would need >1 page of trampoline code; the blob is a few dozen
         * instructions, this is a sanity check, not an expected path. */
        pmm_free_page(tramp_phys);
        kfree(pokes);
        return -(s64)ENOMEM;
    }
    void *tramp_scratch = (void *)PHYS_TO_VIRT(tramp_phys);
    memcpy(tramp_scratch, kexec_trampoline, tramp_len);
    if (tramp_len < PAGE_SIZE) memset((u8 *)tramp_scratch + tramp_len, 0, PAGE_SIZE - tramp_len);

    if (vmm_map(0, KEXEC_TRAMPOLINE_VA, tramp_phys, VMM_KERNEL_RX) != 0) {
        pmm_free_page(tramp_phys);
        kfree(pokes);
        return -(s64)ENOMEM;
    }

    /* ---- 3. Build every forged Limine response and patch it into the
     *         staged image. Every failure from here to the park step is
     *         still completely safe to just return from: nothing has
     *         touched the live page table or any other CPU yet. ---- */
#define ABORT(err) do { rc = (err); goto fail_before_park; } while (0)
#define PATCH(reqfield, respptr) \
    do { if (!kexec_patch_response(&img, &(reqfield).response, (void *)(respptr), &rc)) \
             goto fail_before_park; } while (0)

    /* Framebuffer: physical framebuffer memory does not move, so the new
     * image can just inherit the currently-live response verbatim. */
    {
        struct limine_framebuffer *cur_fb = az_boot_framebuffer();
        struct limine_framebuffer_response *fb_resp = NULL;
        if (cur_fb) {
            struct limine_framebuffer *fb_copy = kmalloc(sizeof(*fb_copy));
            struct limine_framebuffer **fb_arr = kmalloc(sizeof(*fb_arr));
            fb_resp = kmalloc(sizeof(*fb_resp));
            if (!fb_copy || !fb_arr || !fb_resp) ABORT(-(s64)ENOMEM);
            *fb_copy = *cur_fb;
            fb_arr[0] = fb_copy;
            fb_resp->revision = 0;
            fb_resp->framebuffer_count = 1;
            fb_resp->framebuffers = fb_arr;
            RESERVE_PTR(fb_copy);
            RESERVE_PTR(fb_arr);
            RESERVE_PTR(fb_resp);
        }
        PATCH(g_limine_fb_req, fb_resp);
    }

    /* HHDM: same offset, unconditionally — the HHDM mapping is never
     * touched by any part of this kexec. */
    {
        struct limine_hhdm_response *hhdm_resp = kmalloc(sizeof(*hhdm_resp));
        if (!hhdm_resp) ABORT(-(s64)ENOMEM);
        hhdm_resp->revision = 0;
        hhdm_resp->offset = az_boot_hhdm_base();
        RESERVE_PTR(hhdm_resp);
        PATCH(g_limine_hhdm_req, hhdm_resp);
    }

    /* RSDP: ACPI tables are ordinary reserved RAM that does not move. Copy
     * the raw value verbatim — acpi_init() already tolerates either a
     * physical or an HHDM-format address, so whichever form this currently
     * is, the new kernel's acpi_init() will interpret it exactly the same
     * way this one did. */
    {
        struct limine_rsdp_response *rsdp_resp = kmalloc(sizeof(*rsdp_resp));
        if (!rsdp_resp) ABORT(-(s64)ENOMEM);
        rsdp_resp->revision = 0;
        rsdp_resp->address = az_boot_rsdp();
        RESERVE_PTR(rsdp_resp);
        PATCH(g_limine_rsdp_req, rsdp_resp);
    }

    /* Kernel address: report the staged image's own first loaded page —
     * exactly what real Limine would report for this specific ELF (its
     * lowest PT_LOAD is this kernel's _text_start, not literal KERNEL_BASE).
     * Confirmed inert (vmm_init() ignores both fields — see the file
     * banner), but there is no reason to hand back garbage when the honest
     * value is this cheap. */
    {
        struct limine_kernel_address_response *kaddr_resp = kmalloc(sizeof(*kaddr_resp));
        if (!kaddr_resp) ABORT(-(s64)ENOMEM);
        kaddr_resp->revision = 0;
        kaddr_resp->virtual_base = img.pages[0].vaddr;
        kaddr_resp->physical_base = img.pages[0].phys;
        RESERVE_PTR(kaddr_resp);
        PATCH(g_limine_kaddr_req, kaddr_resp);
    }

    /* SMP: forced absent. See kernel/kexec.h and the file banner — the new
     * instance must take its "Limine did not report SMP" fallback path
     * (arch/x86_64/cpu/smp.c, smp_init()) since the APs it would otherwise
     * try to wake are permanently parked, not polling Limine's original
     * (one-time, already-spent) wake structures. */
    PATCH(g_limine_smp_req, NULL);

    /* Modules (initrd): this kernel has no initrd boot mechanism at all —
     * forced absent, not fabricated. */
    PATCH(g_limine_module_req, NULL);

    /* Boot time: non-critical, purely informational — carry the current
     * value forward if Limine ever gave us one, else leave it absent. */
    {
        struct limine_boot_time_response *cur_bt =
            (struct limine_boot_time_response *)g_limine_btime_req.response;
        struct limine_boot_time_response *bt_resp = NULL;
        if (cur_bt) {
            bt_resp = kmalloc(sizeof(*bt_resp));
            if (!bt_resp) ABORT(-(s64)ENOMEM);
            bt_resp->revision = 0;
            bt_resp->boot_time = cur_bt->boot_time;
            RESERVE_PTR(bt_resp);
        }
        PATCH(g_limine_btime_req, bt_resp);
    }

    /* Memory map: copy the live map, then carve out (a) every physical
     * frame backing the staged image itself — so the new kernel's own
     * pmm_init() cannot hand its own running code/data back out as free
     * memory — and (b) every still-parked AP's kernel stack, which remains
     * live (if degraded and inert) after the jump. The old kernel's own
     * [_text_start,_kernel_end) range is *also* carved out for defense in
     * depth, though it is redundant in practice: Limine already reported
     * that exact range as KERNEL_AND_MODULES (not USABLE) at the very first
     * real boot, and nothing here changes that entry. Over-reserving is
     * always safe; under-reserving is corruption.
     *
     * KNOWN LIMITATION (single-generation kexec only): unlike the other
     * forged responses, this response's own backing storage (entry_storage/
     * entry_ptrs/mm_resp below) is deliberately *not* added to
     * extra_reserve — reserving it would mean carving its own physical
     * range out of the very data describing that range, which is circular
     * (the array's size depends on how many carves happened, including its
     * own). Nothing in this kernel ever re-reads a *live* az_boot_memmap()
     * after its one early pmm_init() consumer except kexec_execute() itself,
     * so this is safe for kexec'ing once. Chaining a second kexec_file_load
     * + reboot(RB_KEXEC) from *inside* an already-kexec'd instance is
     * therefore not guaranteed safe — the new kernel's allocator is free to
     * recycle that heap page before a second kexec_execute() call reads it
     * back, which is a real bug this exact codebase hit once during
     * development. Out of scope here: the feature this implements is "jump
     * into a fresh copy," not "support an unbounded chain of jumps." */
    {
        struct limine_memmap_response *base_mm = az_boot_memmap();
        if (!base_mm || base_mm->entry_count == 0) ABORT(-(s64)ENXIO);

        /* Coalesce the staged image's physical frames into contiguous runs. */
        phys_addr_t *phys_sorted = kmalloc(img.page_count * sizeof(phys_addr_t));
        if (!phys_sorted) ABORT(-(s64)ENOMEM);
        for (size_t i = 0; i < img.page_count; i++) phys_sorted[i] = img.pages[i].phys;
        kexec_sort_phys(phys_sorted, 0, (s64)img.page_count - 1);

        u32 cpu_count = smp_cpu_count();
        size_t max_reserved = img.page_count + cpu_count + KEXEC_MAX_TABLE_PAGES
                             + KEXEC_MAX_EXTRA_RESERVE + 2;
        kexec_mm_range_t *reserved = kmalloc(max_reserved * sizeof(*reserved));
        if (!reserved) { kfree(phys_sorted); ABORT(-(s64)ENOMEM); }
        size_t nres = 0;

        size_t i = 0;
        while (i < img.page_count) {
            phys_addr_t run_start = phys_sorted[i];
            phys_addr_t run_end   = run_start + PAGE_SIZE;
            size_t j = i + 1;
            while (j < img.page_count && phys_sorted[j] == run_end) { run_end += PAGE_SIZE; j++; }
            reserved[nres].base = run_start;
            reserved[nres].len  = run_end - run_start;
            reserved[nres].type = LIMINE_MEMMAP_RESERVED;
            nres++;
            i = j;
        }
        kfree(phys_sorted);

        for (u32 c = 0; c < cpu_count; c++) {
            phys_addr_t sbase; size_t slen;
            if (smp_ap_stack_phys(c, &sbase, &slen)) {
                reserved[nres].base = sbase;
                reserved[nres].len  = slen;
                reserved[nres].type = LIMINE_MEMMAP_RESERVED;
                nres++;
            }
        }

        if (g_limine_kaddr_req.response) {
            phys_addr_t old_phys = g_limine_kaddr_req.response->physical_base;
            u64 old_virt = g_limine_kaddr_req.response->virtual_base;
            extern u8 _kernel_end[];
            if ((u64)(uintptr_t)_kernel_end > old_virt) {
                reserved[nres].base = old_phys;
                reserved[nres].len  = (u64)(uintptr_t)_kernel_end - old_virt;
                reserved[nres].type = LIMINE_MEMMAP_RESERVED;
                nres++;
            }
        }

        /* The live kernel-half page table itself — see
         * kexec_collect_live_table_pages()'s doc comment for why this is
         * not optional. One reserved entry per table page found; not worth
         * coalescing (a handful to a few hundred pages, scattered, versus
         * the tens of thousands of image pages above where coalescing
         * actually mattered). */
        {
            phys_addr_t *table_pages = kmalloc(KEXEC_MAX_TABLE_PAGES * sizeof(phys_addr_t));
            if (!table_pages) { kfree(reserved); ABORT(-(s64)ENOMEM); }
            size_t ntable = kexec_collect_live_table_pages(table_pages, KEXEC_MAX_TABLE_PAGES);
            if (ntable > KEXEC_MAX_TABLE_PAGES) {
                /* Truncated — see the cap's doc comment; this would mean a
                 * far larger page-table footprint than any real boot of
                 * this kernel has ever had. Refuse rather than silently
                 * under-reserve a structure whose corruption is fatal. */
                kfree(table_pages);
                kfree(reserved);
                ABORT(-(s64)ENOMEM);
            }
            for (size_t t = 0; t < ntable; t++) {
                reserved[nres].base = table_pages[t];
                reserved[nres].len  = PAGE_SIZE;
                reserved[nres].type = LIMINE_MEMMAP_RESERVED;
                nres++;
            }
            kfree(table_pages);
        }

        /* Every "persistent forged state" allocation collected via
         * RESERVE_PTR() above (framebuffer/HHDM/RSDP/kernel-address/boot-
         * time response objects) — see the doc comment on extra_reserve. */
        for (size_t e = 0; e < extra_reserve_n && nres < max_reserved; e++) {
            reserved[nres++] = extra_reserve[e];
        }

        size_t base_count = base_mm->entry_count;
        size_t cap = base_count + nres + 8;
        kexec_mm_range_t *work = kmalloc(cap * sizeof(*work));
        if (!work) { kfree(reserved); ABORT(-(s64)ENOMEM); }
        size_t wc = 0;
        for (i = 0; i < base_count; i++) {
            work[wc].base = base_mm->entries[i]->base;
            work[wc].len  = base_mm->entries[i]->length;
            work[wc].type = base_mm->entries[i]->type;
            wc++;
        }
        for (i = 0; i < nres; i++) {
            wc = kexec_carve(work, wc, cap, reserved[i].base, reserved[i].len);
        }
        kfree(reserved);

        size_t final_count = 0;
        for (i = 0; i < wc; i++) if (work[i].len > 0) final_count++;
        if (final_count == 0) { kfree(work); ABORT(-(s64)ENXIO); }

        struct limine_memmap_entry *entry_storage = kmalloc(final_count * sizeof(*entry_storage));
        struct limine_memmap_entry **entry_ptrs = kmalloc(final_count * sizeof(*entry_ptrs));
        struct limine_memmap_response *mm_resp = kmalloc(sizeof(*mm_resp));
        if (!entry_storage || !entry_ptrs || !mm_resp) { kfree(work); ABORT(-(s64)ENOMEM); }

        size_t k = 0;
        for (i = 0; i < wc; i++) {
            if (work[i].len == 0) continue;
            entry_storage[k].base = work[i].base;
            entry_storage[k].length = work[i].len;
            entry_storage[k].type = work[i].type;
            entry_ptrs[k] = &entry_storage[k];
            k++;
        }
        kfree(work);

        mm_resp->revision = 0;
        mm_resp->entry_count = final_count;
        mm_resp->entries = entry_ptrs;
        PATCH(g_limine_memmap_req, mm_resp);
    }
#undef PATCH
#undef ABORT
#undef RESERVE_PTR

    /* ---- 4. Park every other CPU. Nothing above this line touched the
     *         live page table or sent an IPI — everything up to here was
     *         a completely safe, retryable, cleanly-abortable sequence. ---- */
    u32 self = smp_current_cpu_id();
    u32 total = smp_cpu_count();
    if (total > 1) {
        __atomic_store_n(&g_kexec_park_count, 0, __ATOMIC_SEQ_CST);
        for (u32 c = 0; c < total; c++) {
            if (c == self) continue;
            smp_send_ipi(c, KEXEC_PARK_VECTOR);
        }
        u64 timeout = KEXEC_PARK_TIMEOUT_ITERS;
        while (__atomic_load_n(&g_kexec_park_count, __ATOMIC_SEQ_CST) < (total - 1) && timeout > 0) {
            cpu_pause();
            timeout--;
        }
        if (__atomic_load_n(&g_kexec_park_count, __ATOMIC_SEQ_CST) < (total - 1)) {
            /* NOTE: any AP that *did* park before this timeout fired has no
             * way back (cli;hlt has no safe "cancel" short of an interrupt,
             * which would defeat parking it in the first place) — this is
             * reported as a clean failure, but a partial park can still
             * permanently cost this boot some CPUs. See kexec_execute()'s
             * doc comment in kernel/kexec.h. */
            kprintf("[KEXEC] timed out waiting for all APs to park (%u/%u parked) — "
                    "aborting kexec, NOT touching the running kernel\n",
                    __atomic_load_n(&g_kexec_park_count, __ATOMIC_SEQ_CST), total - 1);
            return -(s64)EBUSY;
        }
        kprintf("[KEXEC] all %u AP(s) parked\n", total - 1);
    }

    /* ---- 5. Point of no return. Nothing from here on may fail. ---- */
    kprintf("[KEXEC] jumping to new kernel image (entry=0x%016llx)...\n",
            (unsigned long long)img.entry);

    cpu_cli();
    void (*trampoline_fn)(kexec_poke_t *, u64, u64) =
        (void (*)(kexec_poke_t *, u64, u64))(uintptr_t)KEXEC_TRAMPOLINE_VA;
    trampoline_fn(pokes, (u64)img.page_count, img.entry);

    /* Unreachable: the trampoline jumps into the new kernel and never
     * returns. If control somehow lands here, the page-table rewrite left
     * the system in an unknown state — there is nothing safe left to do. */
    PANIC("kexec trampoline returned — should be unreachable");

fail_before_park:
    /* Everything allocated above (pokes, the trampoline mapping, any
     * response objects already patched into the staged image) is simply
     * left in place: the staged image itself (g_kexec_image) is untouched
     * and can still be kexec'd again later, and a stray RX scratch mapping
     * plus a few KB of unreferenced heap is a harmless leak on an aborted,
     * still-fully-running system — not worth the extra bookkeeping to claw
     * back on what is already a rare failure path. */
    return (s64)rc;
}
