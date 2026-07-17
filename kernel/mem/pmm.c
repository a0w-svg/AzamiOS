#include "include/pmm.h"
#include "../klibc/include/string.h"
#include "../klibc/include/stdio.h"
#include "../arch/include/spinlock.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Size of total memory and number of managed blocks (64-bit safe) */
static size_t pmm_memory_size = 0;
static size_t pmm_max_blocks = 0;
static size_t pmm_used_blocks = 0;

/* Pointer to our frame allocation bitmap */
static uint32_t* pmm_bitmap = NULL;

/* ISR-safe spinlock protecting all PMM state and bitmap modifications */
static volatile int pmm_lock = 0;

/* Auxiliary bitmap operations */
static inline void bitmap_set(size_t bit) {
    pmm_bitmap[bit / 32] |= (1UL << (bit % 32));
}

static inline void bitmap_unset(size_t bit) {
    pmm_bitmap[bit / 32] &= ~(1UL << (bit % 32));
}

static inline bool bitmap_test(size_t bit) {
    return (pmm_bitmap[bit / 32] & (1UL << (bit % 32))) != 0;
}

/* Find first free frame index in the bitmap */
static int64_t bitmap_find_first_free(void) {
    size_t words = (pmm_max_blocks + 31) / 32;
    for (size_t i = 0; i < words; i++) {
        if (pmm_bitmap[i] != 0xFFFFFFFFUL) {
            for (int j = 0; j < 32; j++) {
                size_t bit = i * 32 + (size_t)j;
                if (bit >= pmm_max_blocks) {
                    return -1;
                }
                if (!(pmm_bitmap[i] & (1UL << j))) {
                    return (int64_t)bit;
                }
            }
        }
    }
    return -1; /* No available free memory */
}

void pmm_init(size_t mem_size_kb, uintptr_t bitmap_addr) {
    unsigned long flags;
    spinlock_acquire_irqsave(&pmm_lock, &flags);

    pmm_memory_size = (size_t)mem_size_kb * 1024UL;
    pmm_max_blocks = pmm_memory_size / PMM_FRAME_SIZE;
    pmm_used_blocks = pmm_max_blocks;

    pmm_bitmap = (uint32_t*)bitmap_addr;
    
    /* Initialize entire bitmap as occupied (0xFF) initially */
    size_t bitmap_bytes = (pmm_max_blocks + PMM_BLOCKS_PER_BYTE - 1) / PMM_BLOCKS_PER_BYTE;
    memset(pmm_bitmap, 0xFF, bitmap_bytes);

    spinlock_release_irqrestore(&pmm_lock, flags);
}

void pmm_init_region(uintptr_t base_addr, size_t size) {
    if (size == 0) return;

    unsigned long flags;
    spinlock_acquire_irqsave(&pmm_lock, &flags);

    size_t align = base_addr / PMM_FRAME_SIZE;
    size_t blocks = (size + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE;

    for (size_t i = 0; i < blocks; i++) {
        size_t frame = align + i;
        if (frame >= pmm_max_blocks) {
            break; /* Prevent out-of-bounds bitmap write */
        }
        if (!bitmap_test(frame)) {
            bitmap_set(frame);
            pmm_used_blocks++;
        }
    }

    spinlock_release_irqrestore(&pmm_lock, flags);
}

void pmm_deinit_region(uintptr_t base_addr, size_t size) {
    if (size == 0) return;

    unsigned long flags;
    spinlock_acquire_irqsave(&pmm_lock, &flags);

    size_t align = base_addr / PMM_FRAME_SIZE;
    size_t blocks = (size + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE;

    for (size_t i = 0; i < blocks; i++) {
        size_t frame = align + i;
        if (frame >= pmm_max_blocks) {
            break; /* Prevent out-of-bounds bitmap access */
        }
        if (bitmap_test(frame)) {
            bitmap_unset(frame);
            if (pmm_used_blocks > 0) {
                pmm_used_blocks--;
            }
        }
    }

    spinlock_release_irqrestore(&pmm_lock, flags);
}

void* pmm_alloc_block(void) {
    unsigned long flags;
    spinlock_acquire_irqsave(&pmm_lock, &flags);

    if (pmm_used_blocks >= pmm_max_blocks) {
        spinlock_release_irqrestore(&pmm_lock, flags);
        return NULL; /* Out of physical memory */
    }

    int64_t frame = bitmap_find_first_free();
    if (frame == -1 || (size_t)frame >= pmm_max_blocks) {
        spinlock_release_irqrestore(&pmm_lock, flags);
        return NULL;
    }

    bitmap_set((size_t)frame);
    pmm_used_blocks++;

    void* phys_addr = (void*)(uintptr_t)((size_t)frame * PMM_FRAME_SIZE);
    spinlock_release_irqrestore(&pmm_lock, flags);
    return phys_addr;
}

void pmm_free_block(void* addr) {
    if (!addr) {
        return;
    }

    uintptr_t phys = (uintptr_t)addr;
    if ((phys % PMM_FRAME_SIZE) != 0) {
        /* Unaligned physical address passed to pmm_free_block */
        return;
    }

    size_t frame = phys / PMM_FRAME_SIZE;

    unsigned long flags;
    spinlock_acquire_irqsave(&pmm_lock, &flags);

    if (frame >= pmm_max_blocks) {
        /* Out-of-bounds frame address */
        spinlock_release_irqrestore(&pmm_lock, flags);
        return;
    }

    if (!bitmap_test(frame)) {
        /* Double-free protection: frame is already marked free */
        spinlock_release_irqrestore(&pmm_lock, flags);
        return;
    }

    bitmap_unset(frame);
    if (pmm_used_blocks > 0) {
        pmm_used_blocks--;
    }

    spinlock_release_irqrestore(&pmm_lock, flags);
}

size_t pmm_get_free_blocks(void) {
    unsigned long flags;
    spinlock_acquire_irqsave(&pmm_lock, &flags);
    size_t free_blocks = (pmm_max_blocks >= pmm_used_blocks) ? (pmm_max_blocks - pmm_used_blocks) : 0;
    spinlock_release_irqrestore(&pmm_lock, flags);
    return free_blocks;
}

size_t pmm_get_total_blocks(void) {
    unsigned long flags;
    spinlock_acquire_irqsave(&pmm_lock, &flags);
    size_t total = pmm_max_blocks;
    spinlock_release_irqrestore(&pmm_lock, flags);
    return total;
}