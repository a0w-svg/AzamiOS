/* ============================================================================
 * AzamiOS — vDSO image management and per-process mapping
 * File: arch/x86_64/vdso/vdso.c
 *
 * The vDSO image (linux-vdso.so.1, built from vclock.c) is linked into the
 * kernel as raw bytes by vdso_image.asm. vdso_init() copies it once into
 * page-aligned physical frames; vdso_map() then maps those same frames —
 * plus the timekeeper's vvar page and, when present, the HPET counter page —
 * into each new address space at execve():
 *
 *     base - 2 pages   [vvar]  vdso_data                     r--  NX
 *     base - 1 page    [vvar]  HPET MMIO page (UC)           r--  NX
 *     base             [vdso]  ELF image                     r-x
 *
 * All of these frames carry VMM_F_SHARED, so fork() shares rather than
 * copies them and neither munmap() nor exit ever returns them to the PMM.
 * ============================================================================ */

#include "vdso.h"
#include "../../../include/azami/defs.h"
#include "../../../include/azami/vdso.h"
#include "../mm/vmm.h"
#include "../../../kernel/mm/pmm.h"
#include "../../../kernel/mm/vma.h"
#include "../../../kernel/sched/elf.h"
#include "../../../kernel/lib/string.h"
#include "../../../kernel/lib/random.h"
#include "../../../kernel/time/timekeeping.h"
#include "../../../drivers/char/console.h"

extern const u8 vdso_image_start[];
extern const u8 vdso_image_end[];

#define VDSO_MAX_PAGES   4
#define VVAR_PAGES       2          /* data page + HPET page */

/* Top of the vDSO placement window: just below the dynamic linker's
 * randomisation band (INTERP_LOAD_BASE in elf.c) and far above the mmap
 * arena, so neither can land on it. */
#define VDSO_WINDOW_TOP  0x00007ffff5000000ULL
#define VDSO_RND_PAGES   (1U << 12)  /* 16 MB of slide */
#define ADDR_NO_RANDOMIZE 0x0040000U

static phys_addr_t g_vdso_pages[VDSO_MAX_PAGES];
static u32         g_vdso_npages;
static bool        g_vdso_ok;

static bool vdso_validate(const u8 *img, size_t len)
{
    if (len < sizeof(elf64_ehdr_t)) return false;
    const elf64_ehdr_t *eh = (const elf64_ehdr_t *)img;
    if (eh->e_ident_magic != ELF_MAGIC || eh->e_ident_class != ELFCLASS64 ||
        eh->e_machine != EM_X86_64 || eh->e_type != ET_DYN)
        return false;
    if (eh->e_phoff + (u64)eh->e_phnum * sizeof(elf64_phdr_t) > len) return false;

    /* The kernel maps the file verbatim, so the single PT_LOAD must start at
     * offset 0 / vaddr 0 and cover the whole image. */
    const elf64_phdr_t *ph = (const elf64_phdr_t *)(img + eh->e_phoff);
    bool have_load = false;
    for (u16 i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        if (have_load || ph[i].p_offset != 0 || ph[i].p_vaddr != 0 ||
            ph[i].p_filesz > len || ph[i].p_memsz != ph[i].p_filesz)
            return false;
        have_load = true;
    }
    return have_load;
}

void vdso_init(void)
{
    const u8 *img = vdso_image_start;
    size_t len = (size_t)(vdso_image_end - vdso_image_start);

    if (!timekeeping_vvar_phys()) {
        kprintf("[VDSO] No vvar page (timekeeping not initialised); vDSO disabled\n");
        return;
    }
    if (!vdso_validate(img, len)) {
        kprintf("[VDSO] Embedded image is not a valid linux-vdso.so.1; vDSO disabled\n");
        return;
    }

    u32 n = (u32)((len + PAGE_SIZE - 1) / PAGE_SIZE);
    if (n == 0 || n > VDSO_MAX_PAGES) {
        kprintf("[VDSO] Image of %llu bytes exceeds %u pages; vDSO disabled\n",
                (unsigned long long)len, VDSO_MAX_PAGES);
        return;
    }
    for (u32 i = 0; i < n; i++) {
        phys_addr_t p = pmm_alloc_page_zeroed();
        if (!p) {
            for (u32 j = 0; j < i; j++) pmm_free_page(g_vdso_pages[j]);
            kprintf("[VDSO] Out of memory; vDSO disabled\n");
            return;
        }
        size_t off = (size_t)i * PAGE_SIZE;
        size_t chunk = len - off < PAGE_SIZE ? len - off : PAGE_SIZE;
        memcpy(PHYS_TO_VIRT(p), img + off, chunk);
        g_vdso_pages[i] = p;
    }
    g_vdso_npages = n;
    g_vdso_ok = true;
    kprintf("[VDSO] linux-vdso.so.1: %llu bytes in %u page(s)\n",
            (unsigned long long)len, n);
}

u64 vdso_map(struct process *proc, vmm_space_t space)
{
    if (!g_vdso_ok) return 0;

    u64 span = (u64)(VVAR_PAGES + g_vdso_npages) * PAGE_SIZE;
    u64 rnd  = 0;
    if (!(proc->personality & ADDR_NO_RANDOMIZE))
        rnd = (krandom_u64() % VDSO_RND_PAGES) * PAGE_SIZE;
    u64 vvar = VDSO_WINDOW_TOP - span - rnd;
    u64 base = vvar + (u64)VVAR_PAGES * PAGE_SIZE;

    /* Data page: read-only, never executable. */
    if (vmm_map(space, vvar, timekeeping_vvar_phys(),
                VMM_F_PRESENT | VMM_F_USER | VMM_F_NX | VMM_F_SHARED) != 0)
        return 0;

    /* HPET page, uncached, so the vDSO can read the main counter directly
     * whenever the timekeeper runs on (or switches to) the HPET. Mapped
     * even when the TSC is current: a later clocksource switch through
     * sysfs must not strand processes that are already running. */
    phys_addr_t hpet = timekeeping_hpet_page_phys();
    if (hpet &&
        vmm_map(space, vvar + PAGE_SIZE, hpet,
                VMM_F_PRESENT | VMM_F_USER | VMM_F_NX | VMM_F_SHARED |
                VMM_F_PCD | VMM_F_PWT) != 0)
        return 0;

    for (u32 i = 0; i < g_vdso_npages; i++) {
        if (vmm_map(space, base + (u64)i * PAGE_SIZE, g_vdso_pages[i],
                    VMM_F_PRESENT | VMM_F_USER | VMM_F_SHARED) != 0)
            return 0;
    }

    vma_add(proc, vvar, vvar + (hpet ? 2 : 1) * PAGE_SIZE,
            VMA_PROT_READ, VMA_F_VVAR | VMA_F_SHARED);
    vma_add(proc, base, base + (u64)g_vdso_npages * PAGE_SIZE,
            VMA_PROT_READ | VMA_PROT_EXEC, VMA_F_VDSO | VMA_F_SHARED);
    return base;
}
