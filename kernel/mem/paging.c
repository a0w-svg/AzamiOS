#include "include/paging.h"
#include "include/pmm.h"
#include "../klibc/include/string.h"

extern void switch_page_dir(void *page);

// memory for catalogue and first page table must be aligned to 4KB
page_directory_entry_t page_directory[1024] __attribute__((aligned(4096)));
page_table_t page_tables[4] __attribute__((aligned(4096)));

void paging_map_page(uint32_t phys_addr, uint32_t virt_addr, uint8_t is_kernel, uint8_t is_writable){
    uint32_t pd_index = virt_addr >> 22;
    uint32_t pt_index = (virt_addr >> 12) & 0x3FF;

    page_table_t* page_table;
    // check if page table exists
    if(page_directory[pd_index].present == 1){
        //extract table address and shift 12 bits to left.
        page_table = (page_table_t*)(page_directory[pd_index].table << 12);
    }
    else{
        page_table = (page_table_t*)pmm_alloc_block();
        memset(page_table, 0, 4096);

        page_directory[pd_index].table = ((uint32_t)page_table) >> 12;
        page_directory[pd_index].present = 1;
        page_directory[pd_index].writable = 1;
        page_directory[pd_index].user = is_kernel ? 0 : 1;
    }

    page_table->pages[pt_index].frame_addr = phys_addr >> 12;
    page_table->pages[pt_index].present = 1;
    page_table->pages[pt_index].writable = is_writable ? 1 : 0;
    page_table->pages[pt_index].user = is_kernel ? 0 : 1;
}

void paging_init(){
    // clear main page catalogue
    for(int i = 0; i < 1024; i++){
        page_directory[i].value = 0;
        page_directory[i].writable = 1;
    }

    // identity mapping for first 4MB of memory (Kernel runtime protection)
    for(int j = 0; j < 4; j++){
        for(uint32_t i = 0; i < 1024; i++){
            page_tables[j].pages[i].frame_addr = i;
            page_tables[j].pages[i].present = 1;
            page_tables[j].pages[i].writable = 1;
            page_tables[j].pages[i].user = 0; 
        }

        page_directory[j].table = ((uint32_t)&page_tables[j]) >> 12;
        page_directory[j].present = 1;
        page_directory[j].writable = 1;
        page_directory[j].user = 0;
    }
    switch_page_dir(page_directory);
}