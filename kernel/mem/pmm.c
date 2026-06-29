#include "include/pmm.h"
#include "../klibc/include/string.h"
#include <stdint.h>

// Size of total memory and number of its blocks
static uint32_t pmm_memory_size = 0;
static uint32_t pmm_max_blocks = 0;
static uint32_t pmm_used_blocks = 0;

// pointer to our bitmap
static uint32_t* pmm_bitmap = 0;

// auxiliary functions
static inline void bitmap_set(uint32_t bit){
    pmm_bitmap[bit / 32] |= (1 << (bit % 32));
}

static inline void bitmap_unset(uint32_t bit){
    pmm_bitmap[bit / 32] &= ~(1 << (bit % 32));
}


// find first free frame
static int bitmap_find_first_tree(){
    for(uint32_t i = 0; i < pmm_max_blocks / 32; i++){
        if(pmm_bitmap[i] != 0xFFFFFFFF){
            for(int j = 0; j < 32; j++){
                int bit = 1 << j;
                if(!(pmm_bitmap[i] & bit)){
                    return i * 32 + j;
                }
            }
        }
    }
    return -1; // no available free mamory
}

void pmm_init(uint32_t mem_size_kb, uint32_t bitmap_addr){
    pmm_memory_size = mem_size_kb * 1024;
    pmm_max_blocks = pmm_memory_size / PMM_FRAME_SIZE;
    pmm_used_blocks = pmm_max_blocks;

    pmm_bitmap = (uint32_t*)(uintptr_t)bitmap_addr;
    
    memset(pmm_bitmap, 0xFF, pmm_max_blocks / PMM_BLOCKS_PER_BYTE);
}

void pmm_init_region(uint32_t base_addr, uint32_t size){
    uint32_t align = base_addr / PMM_FRAME_SIZE;
    uint32_t blocks = size / PMM_FRAME_SIZE;
    if(size % PMM_FRAME_SIZE){
        blocks++;
    }
    for(uint32_t i = 0; i < blocks; i++){
        bitmap_set(align + i);
        pmm_used_blocks++;
    }
}

void pmm_deinit_region(uint32_t base_addr, uint32_t size){
    uint32_t align = base_addr / PMM_FRAME_SIZE;
    uint32_t blocks = size / PMM_FRAME_SIZE;
    if(size % PMM_FRAME_SIZE){
        blocks++;
    }
    for(uint32_t i = 0; i < blocks; i++){
        bitmap_unset(align + i);
        pmm_used_blocks--;
    }
}

void* pmm_alloc_block(){
    if(pmm_max_blocks - pmm_used_blocks == 0){
        return 0; // no free memory
    }
    int frame = bitmap_find_first_tree();
    if(frame == -1){
        return 0;
    }
    
    if(frame == 0){
        bitmap_set(frame);
        pmm_used_blocks++;
        frame = bitmap_find_first_tree();
    }
    bitmap_set(frame);
    pmm_used_blocks++;
    
    return (void*)(uintptr_t)(frame * PMM_FRAME_SIZE);
}

void pmm_free_block(void* addr){
    uint32_t frame = (uint32_t)(uintptr_t)addr / PMM_FRAME_SIZE;
    bitmap_unset(frame);
    pmm_used_blocks--;
}