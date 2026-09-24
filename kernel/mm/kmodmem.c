/* ============================================================================
 * AzamiOS — Executable Kernel Code Allocator (W^X)
 * File: kernel/mm/kmodmem.c
 *
 * See kmodmem.h for what this exists to stop. The mechanism is deliberately
 * plain: a fixed window of kernel virtual address space, a bitmap over its
 * pages, and frames from the PMM mapped in on demand. There is no sub-page
 * suballocation — a JIT image is rounded up to a page because a page is the
 * granularity at which "writable" and "executable" can be told apart, and
 * sharing one with a second image would mean unsealing that image to write
 * this one.
 * ============================================================================ */

#define DEBUG 0
#include <azami/debug.h>
#include "kmodmem.h"
#include "pmm.h"
#include "../../arch/x86_64/mm/vmm.h"
#include "../../arch/x86_64/cpu/spinlock.h"
#include "../lib/string.h"
#include "../../drivers/char/console.h"
#include "../../include/azami/defs.h"

/*
 * Where runtime-generated kernel code lives: the top gigabyte's lower edge,
 * clear of the kernel image below it and of kexec's trampoline scratch VA.
 *
 * It has to be inside the -2 GB window that -mcmodel=kernel assumes, and not
 * merely somewhere canonical, because generated code calls back into the
 * kernel with rel32 displacements — a helper call from an image mapped
 * outside that window would be a truncated relocation at runtime, which is to
 * say a jump into nowhere. 16 MiB is far more than the BPF JIT will ever ask
 * for (the verifier caps a program at 4096 instructions, and this kernel
 * allows a handful of loaded programs), and the window costs nothing when
 * empty: page tables for it are created on first use.
 */
#define KMOD_BASE        0xFFFFFFFFC0000000ULL
#define KMOD_PAGES       4096                     /* 16 MiB */
#define KMOD_BITMAP_WORDS (KMOD_PAGES / 64)

/* Live allocations. A flat table rather than a list: the count is small and
 * bounded by how many BPF programs can be loaded, and a table keeps free()
 * and seal() from having to trust a header stored in the very memory whose
 * integrity is the point of this file. */
#define KMOD_MAX_ALLOCS  512

typedef struct {
    u64  va;         /* 0 = slot free            */
    u32  pages;
    bool sealed;
} kmod_alloc_t;

static u64          s_bitmap[KMOD_BITMAP_WORDS];
static kmod_alloc_t s_allocs[KMOD_MAX_ALLOCS];
static u64          s_live_bytes = 0;
static spinlock_t   s_lock = SPINLOCK_INIT;

static inline bool bit_test(u32 i)  { return (s_bitmap[i >> 6] >> (i & 63)) & 1; }
static inline void bit_set(u32 i)   { s_bitmap[i >> 6] |=  (1ULL << (i & 63)); }
static inline void bit_clear(u32 i) { s_bitmap[i >> 6] &= ~(1ULL << (i & 63)); }

/* First fit over the page bitmap. Caller holds s_lock. Returns the starting
 * page index, or KMOD_PAGES when no run of @n is available. */
static u32 find_run(u32 n)
{
    u32 run = 0;
    for (u32 i = 0; i < KMOD_PAGES; i++) {
        if (bit_test(i)) { run = 0; continue; }
        if (++run == n) return i + 1 - n;
    }
    return KMOD_PAGES;
}

/* Caller holds s_lock. */
static kmod_alloc_t *find_alloc(u64 va)
{
    for (u32 i = 0; i < KMOD_MAX_ALLOCS; i++)
        if (s_allocs[i].va == va) return &s_allocs[i];
    return NULL;
}

bool kmod_is_exec_addr(const void *p)
{
    u64 va = (u64)(uintptr_t)p;
    return va >= KMOD_BASE && va < KMOD_BASE + (u64)KMOD_PAGES * PAGE_SIZE;
}

u64 kmod_exec_bytes(void)
{
    return __atomic_load_n(&s_live_bytes, __ATOMIC_RELAXED);
}

void *kmod_alloc_exec(size_t len)
{
    if (len == 0) return NULL;
    u64 npages64 = (ALIGN_UP(len, PAGE_SIZE)) / PAGE_SIZE;
    if (npages64 == 0 || npages64 > KMOD_PAGES) return NULL;
    u32 npages = (u32)npages64;

    irqflags_t irqf = spinlock_lock_irqsave(&s_lock);

    kmod_alloc_t *slot = find_alloc(0);
    if (!slot) { spinlock_unlock_irqrestore(&s_lock, irqf); return NULL; }

    u32 first = find_run(npages);
    if (first == KMOD_PAGES) { spinlock_unlock_irqrestore(&s_lock, irqf); return NULL; }

    /* Reserve the range before dropping the lock for the mapping work, so two
     * callers cannot pick the same run. */
    for (u32 i = 0; i < npages; i++) bit_set(first + i);
    slot->va     = KMOD_BASE + (u64)first * PAGE_SIZE;
    slot->pages  = npages;
    slot->sealed = false;
    u64 va_base  = slot->va;

    spinlock_unlock_irqrestore(&s_lock, irqf);

    /* Writable and non-executable to begin with: the caller is about to write
     * code here, and until it says otherwise these pages must not be runnable
     * even by a mistake elsewhere in the kernel. */
    u32 mapped = 0;
    for (; mapped < npages; mapped++) {
        phys_addr_t frame = pmm_alloc_page();
        if (!frame) break;
        if (vmm_map(0, (virt_addr_t)(va_base + (u64)mapped * PAGE_SIZE),
                    frame, VMM_KERNEL_RW) != 0) {
            pmm_free_page(frame);
            break;
        }
        /* Zero through the fresh mapping, not the HHDM alias: same frame,
         * and this is the view the caller will use. */
        memset((void *)(uintptr_t)(va_base + (u64)mapped * PAGE_SIZE), 0, PAGE_SIZE);
    }

    if (mapped != npages) {                      /* partial failure: unwind */
        for (u32 i = 0; i < mapped; i++) {
            virt_addr_t va = (virt_addr_t)(va_base + (u64)i * PAGE_SIZE);
            phys_addr_t p  = vmm_translate(0, va);
            vmm_unmap(0, va);
            if (p) pmm_free_page(p);
        }
        irqf = spinlock_lock_irqsave(&s_lock);
        for (u32 i = 0; i < npages; i++) bit_clear(first + i);
        slot->va = 0;
        spinlock_unlock_irqrestore(&s_lock, irqf);
        return NULL;
    }

    __atomic_add_fetch(&s_live_bytes, (u64)npages * PAGE_SIZE, __ATOMIC_RELAXED);
    pr_debug("[KMOD] alloc %u page(s) at 0x%016llx (RW-)\n",
             npages, (unsigned long long)va_base);
    return (void *)(uintptr_t)va_base;
}

int kmod_seal_exec(void *p)
{
    if (!p || !kmod_is_exec_addr(p)) return -1;
    u64 va = (u64)(uintptr_t)p;

    irqflags_t irqf = spinlock_lock_irqsave(&s_lock);
    kmod_alloc_t *a = find_alloc(va);
    if (!a || a->sealed) { spinlock_unlock_irqrestore(&s_lock, irqf); return -1; }
    u32 pages = a->pages;
    a->sealed = true;
    spinlock_unlock_irqrestore(&s_lock, irqf);

    /* One edit grants execute and revokes write. Doing it as two calls would
     * leave a window — however short, and on an SMP machine another core is
     * genuinely running in it — where the page is both. */
    vmm_protect_range(0, (virt_addr_t)va, (virt_addr_t)(va + (u64)pages * PAGE_SIZE),
                      VMM_F_PRESENT, VMM_F_WRITE | VMM_F_NX, VMM_PROT_SPLIT);

    pr_debug("[KMOD] seal  %u page(s) at 0x%016llx (R-X)\n",
             pages, (unsigned long long)va);
    return 0;
}

void kmod_free_exec(void *p)
{
    if (!p || !kmod_is_exec_addr(p)) return;
    u64 va = (u64)(uintptr_t)p;

    irqflags_t irqf = spinlock_lock_irqsave(&s_lock);
    kmod_alloc_t *a = find_alloc(va);
    if (!a) { spinlock_unlock_irqrestore(&s_lock, irqf); return; }
    u32 pages = a->pages;
    u32 first = (u32)((va - KMOD_BASE) / PAGE_SIZE);
    a->va = 0;
    for (u32 i = 0; i < pages; i++) bit_clear(first + i);
    spinlock_unlock_irqrestore(&s_lock, irqf);

    /* Frames come back only after the mapping is gone and every CPU has been
     * told, which vmm_unmap() handles; reusing a frame that another core can
     * still execute through a stale TLB entry is exactly the bug this
     * allocator exists to avoid. */
    for (u32 i = 0; i < pages; i++) {
        virt_addr_t pva = (virt_addr_t)(va + (u64)i * PAGE_SIZE);
        phys_addr_t ph  = vmm_translate(0, pva);
        vmm_unmap(0, pva);
        if (ph) pmm_free_page(ph);
    }

    __atomic_sub_fetch(&s_live_bytes, (u64)pages * PAGE_SIZE, __ATOMIC_RELAXED);
    pr_debug("[KMOD] free  %u page(s) at 0x%016llx\n",
             pages, (unsigned long long)va);
}

/* ── Boot-time proof that the mechanism works ─────────────────────────────── *
 *
 * Worth having as a real executed test rather than a code review, because
 * both ways this can break are silent. Seal the pages too weakly and the
 * kernel keeps a writable-executable region while /proc reports W^X; seal
 * them too strongly — or let kprotect_seal()'s blanket NX over the direct map
 * catch this window by accident — and nothing notices until the first BPF
 * program is loaded and the kernel jumps into a non-executable page.
 *
 * The test is split around kprotect_seal() rather than run after it, and the
 * reason is measurable. Every page-table edit here ends in a global TLB
 * shootdown, and issuing several of those microseconds apart is exactly the
 * pattern that loses an IPI to the target LAPIC having the vector still in
 * service (see the wait loop in arch/x86_64/mm/tlb.c). Run whole, immediately
 * behind the seal's own shootdown, this test reliably cost about half a
 * second of boot and three alarming "[TLB] shootdown to CPUn stuck" lines —
 * for a diagnostic. Split, all the mapping work happens before the seal, and
 * the half that has to run afterwards is a function call and two reads.
 *
 * The post-seal half is still the half that proves the point: it executes the
 * image again with the direct map now non-executable throughout, which is the
 * configuration that actually ships.
 */

static u8 *s_selftest_img = NULL;

/* mov eax, 0x2A ; ret — the smallest thing with a verifiable answer. */
static const u8 s_selftest_code[] = { 0xB8, 0x2A, 0x00, 0x00, 0x00, 0xC3 };

bool kmod_selftest_prepare(void)
{
    u8 *img = kmod_alloc_exec(sizeof s_selftest_code);
    if (!img) {
        kprintf("[KMOD] selftest: FAIL (allocation)\n");
        return false;
    }

    u64 before = vmm_query_flags(0, (virt_addr_t)(uintptr_t)img);
    if (!(before & VMM_F_WRITE) || !(before & VMM_F_NX)) {
        kprintf("[KMOD] selftest: FAIL (fresh page is not writable-and-NX: "
                "flags=0x%llx)\n", (unsigned long long)before);
        kmod_free_exec(img);
        return false;
    }

    memcpy(img, s_selftest_code, sizeof s_selftest_code);

    if (kmod_seal_exec(img) != 0) {
        kprintf("[KMOD] selftest: FAIL (seal)\n");
        kmod_free_exec(img);
        return false;
    }

    s_selftest_img = img;
    return true;
}

bool kmod_selftest_verify(void)
{
    u8 *img = s_selftest_img;
    if (!img) {
        kprintf("[KMOD] W^X code allocator: FAIL (no image from prepare step)\n");
        return false;
    }

    /* Check the permissions the seal produced before jumping into them — a
     * page that came back still writable would run the code perfectly well
     * and report a pass it had not earned. */
    u64 flags = vmm_query_flags(0, (virt_addr_t)(uintptr_t)img);
    bool wx_ok = (flags & VMM_F_PRESENT) && !(flags & VMM_F_WRITE) && !(flags & VMM_F_NX);

    u32 (*fn)(void) = (u32 (*)(void))(uintptr_t)img;
    u32 got = fn();

    /* Deliberately not freed here. Unmapping costs a global TLB shootdown,
     * and this runs microseconds after kprotect_seal() issued one of its own
     * — the back-to-back case the tlb.c wait loop describes, where the second
     * IPI is dropped by a target LAPIC that still has the vector in service.
     * A parked AP has no other interrupt source to fall back on (its timer
     * does not start until the scheduler does), so the dropped delivery is
     * not noticed until then: measured, this one unmap cost about half a
     * second of boot and three "[TLB] shootdown to CPUn stuck" lines. The
     * page is released by kmod_selftest_release() at the end of kernel_main
     * instead, where nothing else has shot down recently. */
    s_selftest_img = img;

    if (!wx_ok || got != 42) {
        kmod_selftest_release();
        kprintf("[KMOD] W^X code allocator: FAIL (flags=0x%llx w^x=%d returned=%u)\n",
                (unsigned long long)flags, wx_ok, got);
        return false;
    }

    kprintf("[KMOD] W^X code allocator: PASS (wrote, sealed R-X, executed with "
            "heap and direct map NX)\n");
    return true;
}

void kmod_selftest_release(void)
{
    u8 *img = s_selftest_img;
    if (!img) return;
    s_selftest_img = NULL;
    kmod_free_exec(img);
}
