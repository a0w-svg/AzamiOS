#ifndef PMM_H
#define PMM_H

#include <stdint.h>
#include <stdbool.h>

#define PMM_FRAME_SIZE 4096
#define PMM_BLOCKS_PER_BYTE 8
// Initialize bitmap PMM (requires total RAM size in KB)
void  pmm_init(uint32_t mem_size_kb, uint32_t bitmap_addr);

// Set specified memory region as occupied.
void pmm_init_region(uint32_t base_addr, uint32_t size);

// Set memory region as free to use.
void pmm_deinit_region(uint32_t base_addr, uint32_t size);

// Allocate one free frame (4KB) and returns its physical address
void* pmm_alloc_block();

// Free frame under specified physical address
void pmm_free_block(void* addr);
#endif