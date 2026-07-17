#ifndef PMM_H
#define PMM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define PMM_FRAME_SIZE 4096UL
#define PMM_BLOCKS_PER_BYTE 8UL

/**
 * pmm_init - Initialize bitmap-based Physical Memory Manager.
 * @mem_size_kb: Total available system memory in kilobytes.
 * @bitmap_addr: Physical/virtual address where the frame bitmap is stored.
 */
void pmm_init(size_t mem_size_kb, uintptr_t bitmap_addr);

/**
 * pmm_init_region - Mark a physical memory region as occupied (reserved).
 * @base_addr: Base physical address of the region.
 * @size: Size of the region in bytes.
 */
void pmm_init_region(uintptr_t base_addr, size_t size);

/**
 * pmm_deinit_region - Mark a physical memory region as free and available for allocation.
 * @base_addr: Base physical address of the region.
 * @size: Size of the region in bytes.
 */
void pmm_deinit_region(uintptr_t base_addr, size_t size);

/**
 * pmm_alloc_block - Allocate one clean 4KiB page frame from physical memory.
 * Return: Physical address of the allocated frame, or NULL (0) if out of memory.
 */
void* pmm_alloc_block(void);

/**
 * pmm_free_block - Return a 4KiB page frame to the physical memory pool.
 * @addr: Physical address of the frame to free.
 */
void pmm_free_block(void* addr);

/**
 * pmm_get_free_blocks - Query total number of free physical page frames.
 * Return: Number of free 4KiB frames.
 */
size_t pmm_get_free_blocks(void);

/**
 * pmm_get_total_blocks - Query total number of managed physical page frames.
 * Return: Total number of 4KiB frames.
 */
size_t pmm_get_total_blocks(void);

#endif