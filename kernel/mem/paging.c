#include "include/paging.h"
#include "include/pmm.h"
#include "../klibc/include/string.h"
#include "../klibc/include/stdio.h"

extern void switch_page_dir(void *page);

// memory for catalogue and first page table must be aligned to 4KB
page_directory_entry_t page_directory[1024] __attribute__((aligned(4096)));
page_table_t page_tables[32] __attribute__((aligned(4096)));

void paging_map_page(uint32_t phys_addr, uint32_t virt_addr, uint8_t is_kernel, uint8_t is_writable){
    uint32_t pd_index = virt_addr >> 22;
    uint32_t pt_index = (virt_addr >> 12) & 0x3FF;

    page_table_t* page_table = 0;
    // check if page table exists
    if(page_directory[pd_index].present == 1){
        // Extract page table virtual address (identity-mapped, so phys == virt here)
        page_table = (page_table_t*)(page_directory[pd_index].table << 12);
        // x86 requires BOTH the PDE and PTE to have U/S=1 for ring-3 access.
        // If we are mapping a user page into an existing kernel-only page table,
        // we must promote the PDE's user bit so ring-3 can reach the entry.
        if(!is_kernel){
            page_directory[pd_index].user = 1;
        }
    }
    else{
        uint32_t new_table_phys = (uint32_t)pmm_alloc_block();
        if(!new_table_phys){
            kprintf("PANIC: Out of physical memory!\n");
            return;
        }
        page_table = (page_table_t*)new_table_phys;
        memset(page_table, 0, 4096);
            
        page_directory[pd_index].table = new_table_phys >> 12;
        page_directory[pd_index].present = 1;
        page_directory[pd_index].writable = 1;
        page_directory[pd_index].user = is_kernel ? 0 : 1;
    }

    page_table->pages[pt_index].frame_addr = phys_addr >> 12;
    page_table->pages[pt_index].present = 1;
    page_table->pages[pt_index].writable = is_writable ? 1 : 0;
    page_table->pages[pt_index].user = is_kernel ? 0 : 1;

    if(phys_addr >= 0xF0000000){
        page_table->pages[pt_index].pcd = 1;
        page_table->pages[pt_index].pwt = 1;
    }
    asm volatile("invlpg (%0)" : : "b"(virt_addr) : "memory");

    uint32_t cr3;
    asm volatile("mov %%cr3, %0; mov %0, %%cr3" : "=r"(cr3) : "r"(cr3) : "memory");
}

void paging_init(){
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

        page_directory[j].table = ((uint32_t)&page_tables[j]) >> 12;
        page_directory[j].present = 1;
        page_directory[j].writable = 1;
        page_directory[j].user = 0;
    }

    page_directory[1023].table = ((uint32_t)page_directory) >> 12;
    page_directory[1023].present = 1;
    page_directory[1023].writable = 1;
    page_directory[1023].user = 0;
    switch_page_dir(page_directory);
}