#include "include/paging.h"
#include "include/pmm.h"
#include "../klibc/include/string.h"
#include "../klibc/include/stdio.h"
#include <stdbool.h>

extern void switch_page_dir(void *page);

// memory for catalogue and first page table must be aligned to 4KB
page_directory_entry_t page_directory[1024] __attribute__((aligned(4096)));
page_table_t page_tables[32] __attribute__((aligned(4096)));

extern bool g_is_uefi;

#if defined(__x86_64__)
extern uint64_t pd0[];

void paging_map_page(uint32_t phys_addr, uint32_t virt_addr, uint8_t is_kernel, uint8_t is_writable){
    uint32_t pd_index = virt_addr >> 21;
    if (pd_index >= 2048) return;

    if (pd0[pd_index] & 0x80u) {
        if ((phys_addr & ~0xFFFu) == (virt_addr & ~0xFFFu)) {
            return; /* Already identity mapped by 2MB huge page */
        }
        uint64_t *pt = (uint64_t*)pmm_alloc_block();
        if (!pt) {
            kprintf("PANIC: Out of physical memory in 64-bit paging!\n");
            return;
        }
        uint64_t base_phys = (uint64_t)pd_index << 21;
        for (int i = 0; i < 512; i++) {
            pt[i] = (base_phys + i * 4096) | 0x07u;
        }
        pd0[pd_index] = ((uint64_t)(uintptr_t)pt) | 0x07u;
    }

    uint64_t *pt = (uint64_t*)(uintptr_t)(pd0[pd_index] & ~0xFFFu);
    uint32_t pt_index = (virt_addr >> 12) & 0x1FF;
    pt[pt_index] = (phys_addr & ~0xFFFu)
        | (is_writable ? 0x02u : 0x00u)
        | 0x01u
        | (is_kernel ? 0x00u : 0x04u);

    asm volatile("invlpg (%0)" : : "r"((uintptr_t)virt_addr) : "memory");
}

void paging_map_framebuffer(uint32_t lfb_phys, uint32_t size_bytes) {
    (void)lfb_phys; (void)size_bytes;
    /* Framebuffer is below 4GB, already identity mapped by boot64.asm */
}
#else
void paging_map_page(uint32_t phys_addr, uint32_t virt_addr, uint8_t is_kernel, uint8_t is_writable){
    uint32_t pd_index = virt_addr >> 22;
    uint32_t pt_index = (virt_addr >> 12) & 0x3FF;

    page_table_t* page_table = 0;
    // check if page table exists
    if(page_directory[pd_index].present == 1){
        // Extract page table virtual address (identity-mapped, so phys == virt here)
        page_table = (page_table_t*)(uintptr_t)(page_directory[pd_index].table << 12);
        // x86 requires BOTH the PDE and PTE to have U/S=1 for ring-3 access.
        // If we are mapping a user page into an existing kernel-only page table,
        // we must promote the PDE's user bit so ring-3 can reach the entry.
        if(!is_kernel){
            page_directory[pd_index].user = 1;
        }
    }
    else{
        uint32_t new_table_phys = (uint32_t)(uintptr_t)pmm_alloc_block();
        if(!new_table_phys){
            kprintf("PANIC: Out of physical memory!\n");
            return;
        }
        page_table = (page_table_t*)(uintptr_t)new_table_phys;
        memset(page_table, 0, 4096);
            
        page_directory[pd_index].table = new_table_phys >> 12;
        page_directory[pd_index].present = 1;
        page_directory[pd_index].writable = 1;
        page_directory[pd_index].user = is_kernel ? 0 : 1;
    }

    page_table->pages[pt_index].value = (phys_addr & 0xFFFFF000u)
        | (is_writable ? 0x02u : 0x00u)
        | 0x01u
        | (is_kernel ? 0x00u : 0x04u);

    asm volatile("invlpg (%0)" : : "b"(virt_addr) : "memory");
    switch_page_dir(page_directory);
}

/* Static page table for the MMIO linear framebuffer (avoids PMM-allocated PT issues). */
static page_table_t g_lfb_pt __attribute__((aligned(4096)));

void paging_map_framebuffer(uint32_t lfb_phys, uint32_t size_bytes) {
    uint32_t pages = (size_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    if (pages > 1024) pages = 1024;

    memset(&g_lfb_pt, 0, sizeof(g_lfb_pt));
    for (uint32_t i = 0; i < pages; i++) {
        g_lfb_pt.pages[i].value = (lfb_phys + (i * PAGE_SIZE)) | 0x03u;
    }

    uint32_t pd_index = lfb_phys >> 22;
    page_directory[pd_index].value = ((uint32_t)(uintptr_t)&g_lfb_pt) | 0x03u;
    switch_page_dir(page_directory);
}
#endif

void paging_init(){
#if defined(__x86_64__)
    return;
#endif
    if (g_is_uefi) return; /* Keep UEFI firmware 4-level PML4 paging in 64-bit mode */

    // clear main page catalogue
    for(int i = 0; i < 1024; i++){
        page_directory[i].value = 0;
        page_directory[i].writable = 1;
    }

    // identity mapping for first 128MB of memory (Kernel runtime protection)
    for(int j = 0; j < 32; j++){
        for(uint32_t i = 0; i < 1024; i++){
            page_tables[j].pages[i].frame_addr = (j * 1024) + i;
            page_tables[j].pages[i].present = 1;
            page_tables[j].pages[i].writable = 1;
            page_tables[j].pages[i].user = 0; 
        }

        page_directory[j].table = ((uint32_t)(uintptr_t)&page_tables[j]) >> 12;
        page_directory[j].present = 1;
        page_directory[j].writable = 1;
        page_directory[j].user = 0;
    }

    page_directory[1023].table = ((uint32_t)(uintptr_t)page_directory) >> 12;
    page_directory[1023].present = 1;
    page_directory[1023].writable = 1;
    page_directory[1023].user = 0;
    switch_page_dir(page_directory);
}