#ifndef PAGING_H
#define PAGING_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define PAGE_SIZE 4096UL

typedef union
{
    uint32_t value;
    struct
    {
        /* Present flag: must be 1 to map a 4KB page */
        uint32_t present    : 1;
        /* Read/Write flag: 0 for read-only, 1 for read/write */
        uint32_t writable   : 1;
        /* User/Supervisor flag: 0 for kernel-only, 1 for user-accessible */
        uint32_t user       : 1;
        /* Page-level write-through */
        uint32_t pwt        : 1;
        /* Page-level cache disable */
        uint32_t pcd        : 1;
        uint32_t accessed   : 1;
        uint32_t dirty      : 1;
        uint32_t pat        : 1;
        uint32_t global     : 1;
        uint32_t unused     : 3;
        uint32_t frame_addr : 20;
    }; 
} page_t;

typedef struct page_table
{
    page_t pages[1024];
} page_table_t;

typedef union 
{
    uint32_t value;
    struct 
    {
        uint32_t present        : 1;
        uint32_t writable       : 1;
        uint32_t user           : 1;
        uint32_t pwt            : 1;
        uint32_t pcd            : 1;
        uint32_t accessed       : 1;
        uint32_t unused         : 6;
        uint32_t table          : 20;
    };
} page_directory_entry_t;

/**
 * paging_map_page - Map a physical 4KiB page frame to a virtual address.
 * @phys_addr: Physical frame address (must be 4KiB aligned).
 * @virt_addr: Target virtual address (must be 4KiB aligned).
 * @is_kernel: 1 for kernel-only (ring-0), 0 for user-accessible (ring-3).
 * @is_writable: 1 for read-write, 0 for read-only.
 * Return: 0 on success, -1 on allocation/mapping failure (OOM).
 */
int paging_map_page(uintptr_t phys_addr, uintptr_t virt_addr, uint8_t is_kernel, uint8_t is_writable);

/**
 * paging_unmap_page - Remove virtual mapping and invalidate TLB.
 * @virt_addr: Virtual address to unmap.
 * Return: 0 on success, -1 if page was not mapped.
 */
int paging_unmap_page(uintptr_t virt_addr);

/**
 * paging_get_phys - Translate a virtual address to physical address via page walks.
 * @virt_addr: Virtual address to translate.
 * Return: Physical address corresponding to virt_addr, or 0 if unmapped.
 */
uintptr_t paging_get_phys(uintptr_t virt_addr);

/**
 * paging_map_framebuffer - Identity map or map linear framebuffer region.
 */
void paging_map_framebuffer(uint32_t lfb_phys, uint32_t size_bytes);

/**
 * paging_init - Initialize base page directory and identity maps (for 32-bit/non-UEFI boot).
 */
void paging_init(void);

/**
 * paging_clone_directory - Create a deep copy of current page directory for process cloning.
 * Return: Physical address of cloned root directory/PML4.
 */
uintptr_t paging_clone_directory(void);

#endif
