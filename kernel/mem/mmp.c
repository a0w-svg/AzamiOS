#include "include/mmp.h"
#include "include/pmm.h"
#include "include/paging.h"
#include "../klibc/include/string.h"
#include "../klibc/include/stdio.h"
#include "../arch/include/spinlock.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define HEAP_MAGIC       0x415A414D494F5348ULL /* "AZAMIOSH" in ASCII */
#define HEAP_ALIGNMENT   16UL
#define HEAP_MIN_PAYLOAD 16UL

typedef struct block_header {
    uint64_t magic;            /* 8 bytes: Magic number canary to detect heap smashing */
    size_t size;               /* 8 bytes: Size of payload without header (strictly multiple of 16) */
    uint32_t is_free;          /* 4 bytes: 1 if free, 0 if occupied */
    uint32_t flags;            /* 4 bytes: Reserved status flags */
    struct block_header* next; /* 8 bytes: Pointer to next header in virtual address order */
    struct block_header* prev; /* 8 bytes: Pointer to previous header in virtual address order */
} __attribute__((aligned(16))) block_header_t;

static uintptr_t heap_start = 0xFFFF8000C0000000ULL;
static uintptr_t heap_max   = 0xFFFF8000F0000000ULL;

static uintptr_t heap_curr = 0;
static block_header_t* head = NULL;
static block_header_t* tail = NULL;

/* ISR-safe spinlock protecting all heap allocations, splits, and coalescing */
static volatile int heap_lock = 0;

/* Accounting statistics */
static size_t heap_used_bytes = 0;
static size_t heap_free_bytes = 0;

void kheap_init(void) {
    unsigned long flags;
    spinlock_acquire_irqsave(&heap_lock, &flags);
    heap_curr = heap_start;
    head = NULL;
    tail = NULL;
    heap_used_bytes = 0;
    heap_free_bytes = 0;
    spinlock_release_irqrestore(&heap_lock, flags);
}

static inline void split_block(block_header_t* block, size_t aligned_size) {
    if (block->size >= aligned_size + sizeof(block_header_t) + HEAP_MIN_PAYLOAD) {
        size_t remaining_size = block->size - aligned_size - sizeof(block_header_t);
        block_header_t* split_hdr = (block_header_t*)((uintptr_t)block + sizeof(block_header_t) + aligned_size);
        
        split_hdr->magic = HEAP_MAGIC;
        split_hdr->size = remaining_size;
        split_hdr->is_free = 1;
        split_hdr->flags = 0;
        split_hdr->next = block->next;
        split_hdr->prev = block;

        if (block->next) {
            block->next->prev = split_hdr;
        } else if (tail == block) {
            tail = split_hdr;
        }
        block->next = split_hdr;
        block->size = aligned_size;
        
        heap_free_bytes -= sizeof(block_header_t);
    }
}

void* kmalloc(size_t size) {
    if (size == 0 || size > (UINTPTR_MAX - sizeof(block_header_t) - PAGE_SIZE)) {
        return NULL;
    }

    if (heap_curr == 0) {
        kheap_init();
    }

    /* 1. STRICT ALIGNMENT: Ensure payload size rounded up to 16-byte boundary */
    size_t aligned_size = (size + (HEAP_ALIGNMENT - 1)) & ~(HEAP_ALIGNMENT - 1);
    if (aligned_size < HEAP_MIN_PAYLOAD) {
        aligned_size = HEAP_MIN_PAYLOAD;
    }

    unsigned long flags;
    spinlock_acquire_irqsave(&heap_lock, &flags);

    /* 2. First Fit search in existing free blocks */
    block_header_t* current = head;
    while (current) {
        if (current->magic != HEAP_MAGIC) {
            kprintf("kmalloc: PANIC/Security alert — Corrupted heap block magic 0x%x at 0x%x!\n",
                    (uint32_t)current->magic, (uint32_t)(uintptr_t)current);
            spinlock_release_irqrestore(&heap_lock, flags);
            return NULL;
        }
        if (current->is_free && current->size >= aligned_size) {
            split_block(current, aligned_size);
            current->is_free = 0;
            heap_free_bytes -= current->size;
            heap_used_bytes += current->size;
            spinlock_release_irqrestore(&heap_lock, flags);
            return (void*)((uintptr_t)current + sizeof(block_header_t));
        }
        current = current->next;
    }

    /* 3. PAGE BOUNDARY PROTECTION: Request clean 4KiB page-aligned blocks from VMM */
    size_t total_needed = aligned_size + sizeof(block_header_t);
    size_t pages = (total_needed + PAGE_SIZE - 1) / PAGE_SIZE;
    size_t alloc_bytes = pages * PAGE_SIZE;

    if (heap_curr + alloc_bytes > heap_max || heap_curr + alloc_bytes < heap_curr) {
        spinlock_release_irqrestore(&heap_lock, flags);
        return NULL; /* Out of Memory: Graceful return without kernel panic */
    }

    uintptr_t map_addr = heap_curr;
    size_t pages_mapped = 0;
    bool oom = false;

    for (size_t i = 0; i < pages; i++) {
        void* phys = pmm_alloc_block();
        if (!phys) {
            oom = true;
            break;
        }
        if (paging_map_page((uintptr_t)phys, map_addr + (i * PAGE_SIZE), 1, 1) != 0) {
            pmm_free_block(phys);
            oom = true;
            break;
        }
        pages_mapped++;
    }

    /* Rollback cleanly if partial page allocation or mapping failed */
    if (oom) {
        for (size_t i = 0; i < pages_mapped; i++) {
            uintptr_t vaddr = map_addr + (i * PAGE_SIZE);
            uintptr_t paddr = paging_get_phys(vaddr);
            if (paddr) {
                pmm_free_block((void*)paddr);
            }
            paging_unmap_page(vaddr);
        }
        spinlock_release_irqrestore(&heap_lock, flags);
        return NULL; /* Graceful OOM handling */
    }

    /* Create block header for newly mapped virtual region */
    block_header_t* new_block = (block_header_t*)map_addr;
    new_block->magic = HEAP_MAGIC;
    new_block->size = alloc_bytes - sizeof(block_header_t);
    new_block->is_free = 1;
    new_block->flags = 0;
    new_block->next = NULL;
    new_block->prev = tail;

    if (tail) {
        tail->next = new_block;
    } else {
        head = new_block;
    }
    tail = new_block;
    heap_curr += alloc_bytes;
    heap_free_bytes += new_block->size;

    /* Coalesce across page boundary if the previous block right before expansion was free */
    if (new_block->prev && new_block->prev->is_free) {
        block_header_t* prev_blk = new_block->prev;
        if (prev_blk->magic == HEAP_MAGIC &&
            (uintptr_t)prev_blk + sizeof(block_header_t) + prev_blk->size == (uintptr_t)new_block) {
            prev_blk->size += sizeof(block_header_t) + new_block->size;
            prev_blk->next = NULL;
            tail = prev_blk;
            new_block->magic = 0;
            new_block = prev_blk;
        }
    }

    /* Allocate from the newly expanded/coalesced region */
    split_block(new_block, aligned_size);
    new_block->is_free = 0;
    heap_free_bytes -= new_block->size;
    heap_used_bytes += new_block->size;

    spinlock_release_irqrestore(&heap_lock, flags);
    return (void*)((uintptr_t)new_block + sizeof(block_header_t));
}

void kfree(void* ptr) {
    if (!ptr) {
        return;
    }

    unsigned long flags;
    spinlock_acquire_irqsave(&heap_lock, &flags);

    uintptr_t ptr_val = (uintptr_t)ptr;

    /* 4. COALESCING SECURITY: Rigorous boundary checks & alignment verification */
    if (ptr_val < heap_start + sizeof(block_header_t) || ptr_val >= heap_curr) {
        kprintf("kfree: Security violation — Pointer 0x%x outside valid heap range!\n", (uint32_t)ptr_val);
        spinlock_release_irqrestore(&heap_lock, flags);
        return;
    }
    if ((ptr_val % HEAP_ALIGNMENT) != 0) {
        kprintf("kfree: Security violation — Unaligned pointer 0x%x!\n", (uint32_t)ptr_val);
        spinlock_release_irqrestore(&heap_lock, flags);
        return;
    }

    block_header_t* block = (block_header_t*)(ptr_val - sizeof(block_header_t));

    /* Verify magic number canary */
    if (block->magic != HEAP_MAGIC) {
        kprintf("kfree: Security violation — Heap corruption / invalid magic at block 0x%x!\n",
                (uint32_t)(uintptr_t)block);
        spinlock_release_irqrestore(&heap_lock, flags);
        return;
    }

    /* Double-free protection */
    if (block->is_free) {
        kprintf("kfree: Security violation — Double-free detected at block 0x%x!\n",
                (uint32_t)(uintptr_t)block);
        spinlock_release_irqrestore(&heap_lock, flags);
        return;
    }

    block->is_free = 1;
    heap_used_bytes -= block->size;
    heap_free_bytes += block->size;

    /* Coalesce rightward with next block if contiguous and free */
    if (block->next && block->next->is_free) {
        block_header_t* next_blk = block->next;

        /* Rigorous boundary and reciprocal linkage verification before merging */
        if (next_blk->magic != HEAP_MAGIC) {
            kprintf("kfree: Security violation — Next block magic corrupted during coalescing!\n");
            spinlock_release_irqrestore(&heap_lock, flags);
            return;
        }
        if (next_blk->prev != block) {
            kprintf("kfree: Security violation — Heap linkage mismatch (next->prev != block) during coalescing!\n");
            spinlock_release_irqrestore(&heap_lock, flags);
            return;
        }
        if ((uintptr_t)block + sizeof(block_header_t) + block->size != (uintptr_t)next_blk) {
            kprintf("kfree: Security violation — Non-contiguous block address during coalescing!\n");
            spinlock_release_irqrestore(&heap_lock, flags);
            return;
        }
        if (block->size > UINTPTR_MAX - sizeof(block_header_t) - next_blk->size) {
            kprintf("kfree: Security violation — Integer/pointer wrapper overflow during coalescing!\n");
            spinlock_release_irqrestore(&heap_lock, flags);
            return;
        }

        block->size += sizeof(block_header_t) + next_blk->size;
        block->next = next_blk->next;
        if (block->next) {
            if (block->next->prev != next_blk) {
                kprintf("kfree: Security violation — Forward linkage mismatch during coalescing!\n");
                spinlock_release_irqrestore(&heap_lock, flags);
                return;
            }
            block->next->prev = block;
        } else if (tail == next_blk) {
            tail = block;
        }
        next_blk->magic = 0; /* Scrub merged header canary */
        heap_free_bytes += sizeof(block_header_t);
    }

    /* Coalesce leftward with previous block if contiguous and free */
    if (block->prev && block->prev->is_free) {
        block_header_t* prev_blk = block->prev;

        if (prev_blk->magic != HEAP_MAGIC) {
            kprintf("kfree: Security violation — Previous block magic corrupted during left coalescing!\n");
            spinlock_release_irqrestore(&heap_lock, flags);
            return;
        }
        if (prev_blk->next != block) {
            kprintf("kfree: Security violation — Heap linkage mismatch (prev->next != block) during left coalescing!\n");
            spinlock_release_irqrestore(&heap_lock, flags);
            return;
        }
        if ((uintptr_t)prev_blk + sizeof(block_header_t) + prev_blk->size != (uintptr_t)block) {
            kprintf("kfree: Security violation — Non-contiguous block address during left coalescing!\n");
            spinlock_release_irqrestore(&heap_lock, flags);
            return;
        }
        if (prev_blk->size > UINTPTR_MAX - sizeof(block_header_t) - block->size) {
            kprintf("kfree: Security violation — Integer/pointer wrapper overflow during left coalescing!\n");
            spinlock_release_irqrestore(&heap_lock, flags);
            return;
        }

        prev_blk->size += sizeof(block_header_t) + block->size;
        prev_blk->next = block->next;
        if (block->next) {
            if (block->next->prev != block) {
                kprintf("kfree: Security violation — Forward linkage mismatch during left coalescing!\n");
                spinlock_release_irqrestore(&heap_lock, flags);
                return;
            }
            block->next->prev = prev_blk;
        } else if (tail == block) {
            tail = prev_blk;
        }
        block->magic = 0; /* Scrub merged header canary */
        heap_free_bytes += sizeof(block_header_t);
    }

    spinlock_release_irqrestore(&heap_lock, flags);
}

size_t kheap_get_used_bytes(void) {
    unsigned long flags;
    spinlock_acquire_irqsave(&heap_lock, &flags);
    size_t used = heap_used_bytes;
    spinlock_release_irqrestore(&heap_lock, flags);
    return used;
}

size_t kheap_get_free_bytes(void) {
    unsigned long flags;
    spinlock_acquire_irqsave(&heap_lock, &flags);
    size_t free_b = heap_free_bytes;
    spinlock_release_irqrestore(&heap_lock, flags);
    return free_b;
}