/* ============================================================================
 * AzamiOS — Kernel Memory Allocator Implementation (Bucket/Slab Allocator)
 * File: kernel/mm/kmalloc.c
 * ============================================================================ */

#define DEBUG 1
#include <azami/debug.h>
#include "kmalloc.h"
#include "pmm.h"
#include "../../arch/x86_64/mm/vmm.h"
#include "../../arch/x86_64/cpu/spinlock.h"
#include "../../drivers/char/console.h"
#include "../../include/azami/defs.h"


#define BUCKET_COUNT  8
#define MIN_BUCKET_SHIFT 5  /* 2^5 = 32 bytes */

/* Allocation header prepended to every allocation */
typedef struct block_hdr {
    u32 magic;         /* Magic number for validation: 0x4B4D414C ("KMAL") */
    u32 bucket_idx;    /* Bucket index (0..8) or 0xFF if large allocation */
    u64 size;          /* Requested size or total allocated pages if large */
} block_hdr_t;

#define KMALLOC_MAGIC  0x4B4D414CU

typedef struct free_block {
    struct free_block *next;
} free_block_t;

typedef struct {
    free_block_t *free_list;
    spinlock_t    lock;
    size_t        block_size;
} bucket_t;

static bucket_t g_buckets[BUCKET_COUNT];
static spinlock_t g_large_lock = SPINLOCK_INIT;

void kmalloc_init(void)
{
    for (int i = 0; i < BUCKET_COUNT; i++) {
        g_buckets[i].free_list = NULL;
        g_buckets[i].lock = (spinlock_t)SPINLOCK_INIT;
        g_buckets[i].block_size = (1UL << (i + MIN_BUCKET_SHIFT));
    }
    pr_debug("[KMALLOC] Bucket allocator initialized (32B to 4KB pools)\n");
}

static int size_to_bucket(size_t size)
{
    size_t total = size + sizeof(block_hdr_t);
    if (total < size) return -1; /* Integer overflow */
    
    for (int i = 0; i < BUCKET_COUNT; i++) {
        if (total <= g_buckets[i].block_size) return i;
    }
    return -1;
}

void *kmalloc(size_t size)
{
    if (size == 0) return NULL;

    int idx = size_to_bucket(size);

    /* For requests larger than the largest bucket, allocate full pages directly */
    if (idx < 0) {
        /* Prevent absurdly large allocations (e.g. > 1 GB) */
        if (size > (1024ULL * 1024 * 1024)) return NULL;
        
        size_t total_size = size + sizeof(block_hdr_t);
        if (total_size < size) return NULL; /* Integer overflow */
        
        size_t pages = (total_size + PAGE_SIZE - 1) / PAGE_SIZE;

        irqflags_t flags = spinlock_lock_irqsave(&g_large_lock);
        phys_addr_t phys = pmm_alloc_pages(pages);
        spinlock_unlock_irqrestore(&g_large_lock, flags);

        if (!phys) return NULL;

        block_hdr_t *hdr = (block_hdr_t *)PHYS_TO_VIRT(phys);
        hdr->magic = KMALLOC_MAGIC;
        hdr->bucket_idx = 0xFF;
        hdr->size = size; /* H-05: store *requested size*, not page count */

        return (void *)(hdr + 1);
    }

    bucket_t *b = &g_buckets[idx];
    irqflags_t flags = spinlock_lock_irqsave(&b->lock);

    if (!b->free_list) {
        /* PERF-03: Release bucket lock before calling pmm_alloc_page() to avoid
         * holding two locks simultaneously (bucket -> pmm). Build the slice list
         * locally and re-acquire the lock only to splice it in. */
        spinlock_unlock_irqrestore(&b->lock, flags);

        phys_addr_t page = pmm_alloc_page();
        if (!page) return NULL;

        u8 *virt = (u8 *)PHYS_TO_VIRT(page);
        size_t blk_size = b->block_size;
        size_t count = PAGE_SIZE / blk_size;

        /* Build local free list from the new page */
        free_block_t *local_head = NULL;
        for (size_t j = 0; j < count; j++) {
            free_block_t *blk = (free_block_t *)(virt + j * blk_size);
            blk->next = local_head;
            local_head = blk;
        }

        /* Re-acquire bucket lock to splice in the new blocks */
        flags = spinlock_lock_irqsave(&b->lock);
        /* Another CPU may have refilled while we were unlocked; append ours anyway */
        free_block_t *tail = local_head;
        while (tail->next) tail = tail->next;
        tail->next = b->free_list;
        b->free_list = local_head;
    }

    free_block_t *blk = b->free_list;
    b->free_list = blk->next;
    spinlock_unlock_irqrestore(&b->lock, flags);

    block_hdr_t *hdr = (block_hdr_t *)blk;
    hdr->magic = KMALLOC_MAGIC;
    hdr->bucket_idx = (u32)idx;
    hdr->size = size;

    return (void *)(hdr + 1);
}

void *kzalloc(size_t size)
{
    void *ptr = kmalloc(size);
    if (ptr) {
        __builtin_memset(ptr, 0, size);
    }
    return ptr;
}

void *krealloc(void *ptr, size_t new_size)
{
    if (!ptr) return kmalloc(new_size);
    if (new_size == 0) {
        kfree(ptr);
        return NULL;
    }

    block_hdr_t *hdr = ((block_hdr_t *)ptr) - 1;
    if (hdr->magic != KMALLOC_MAGIC) {
        PANIC("krealloc called on corrupted or non-kmalloc pointer!");
    }

    bool same_bucket = (hdr->bucket_idx == 0xFF) ? (size_to_bucket(new_size) == -1) : (size_to_bucket(new_size) == (int)hdr->bucket_idx);
    
    /* H-05: hdr->size now always stores the requested size for both bucket and large allocs */
    size_t old_size = hdr->size;
    
    if (new_size <= old_size && same_bucket) {
        if (hdr->bucket_idx != 0xFF) hdr->size = new_size;
        return ptr;
    }

    void *new_ptr = kmalloc(new_size);
    if (!new_ptr) return NULL;

    size_t copy_len = (old_size < new_size) ? old_size : new_size;
    __builtin_memcpy(new_ptr, ptr, copy_len);
    kfree(ptr);

    return new_ptr;
}

void kfree(void *ptr)
{
    if (!ptr) return;

    block_hdr_t *hdr = ((block_hdr_t *)ptr) - 1;
    if (hdr->magic != KMALLOC_MAGIC) {
        pr_debug("[KMALLOC] Corrupted kfree ptr=%p, hdr=%p, magic=0x%x (expected 0x%x), caller=%p\n",
                 ptr, hdr, (unsigned int)hdr->magic, (unsigned int)KMALLOC_MAGIC, __builtin_return_address(0));
        PANIC("kfree called on corrupted or non-kmalloc pointer!");
    }

    hdr->magic = 0; /* Invalidate magic to catch double-free */

    if (hdr->bucket_idx == 0xFF) {
        /* H-05: hdr->size is the requested size; recompute page count for freeing */
        size_t total = hdr->size + sizeof(block_hdr_t);
        size_t pages = (total + PAGE_SIZE - 1) / PAGE_SIZE;
        irqflags_t flags = spinlock_lock_irqsave(&g_large_lock);
        pmm_free_pages(VIRT_TO_PHYS((virt_addr_t)hdr), pages);
        spinlock_unlock_irqrestore(&g_large_lock, flags);
        return;
    }

    u32 idx = hdr->bucket_idx;
    if (idx >= BUCKET_COUNT) {
        PANIC("kfree invalid bucket index!");
    }

    bucket_t *b = &g_buckets[idx];
    free_block_t *blk = (free_block_t *)hdr;

    irqflags_t flags = spinlock_lock_irqsave(&b->lock);
    blk->next = b->free_list;
    b->free_list = blk;
    spinlock_unlock_irqrestore(&b->lock, flags);
}
