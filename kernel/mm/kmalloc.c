/* ============================================================================
 * AzamiOS — Kernel Memory Allocator Implementation (Bucket/Slab Allocator)
 * File: kernel/mm/kmalloc.c
 * ============================================================================ */

#define DEBUG 0
#include <azami/debug.h>
#include "kmalloc.h"
#include "pmm.h"
#include "../../arch/x86_64/mm/vmm.h"
#include "../../arch/x86_64/cpu/spinlock.h"
#include "../../drivers/char/console.h"
#include "../../include/azami/defs.h"
#include "../sched/sched.h"


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
    size_t        pages;        /* PMM pages this bucket currently holds */
} bucket_t;

static bucket_t g_buckets[BUCKET_COUNT];
static spinlock_t g_large_lock = SPINLOCK_INIT;

/* Page accounting for /proc/meminfo. Relaxed atomic; a transiently skewed read
 * never matters. Per-bucket page counts live in bucket_t::pages (under that
 * bucket's lock); g_large_pages counts pages pinned by above-bucket allocs. */
static u64 g_large_pages;

void kmalloc_init(void)
{
    for (int i = 0; i < BUCKET_COUNT; i++) {
        g_buckets[i].free_list = NULL;
        g_buckets[i].lock = (spinlock_t)SPINLOCK_INIT;
        g_buckets[i].block_size = (1UL << (i + MIN_BUCKET_SHIFT));
        g_buckets[i].pages = 0;
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
        __atomic_add_fetch(&g_large_pages, pages, __ATOMIC_RELAXED);

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
        b->pages++;
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

void *kcalloc(size_t nmemb, size_t size)
{
    if (nmemb != 0 && size > ((size_t)-1) / nmemb) return NULL; /* overflow */
    return kzalloc(nmemb * size);
}

size_t ksize(const void *ptr)
{
    if (!ptr) return 0;

    const block_hdr_t *hdr = ((const block_hdr_t *)ptr) - 1;
    if (hdr->magic != KMALLOC_MAGIC) return 0;

    if (hdr->bucket_idx == 0xFF) {
        /* Large allocation: the mapping spans whole pages. */
        size_t total = hdr->size + sizeof(block_hdr_t);
        size_t pages = (total + PAGE_SIZE - 1) / PAGE_SIZE;
        return pages * PAGE_SIZE - sizeof(block_hdr_t);
    }

    if (hdr->bucket_idx >= BUCKET_COUNT) return 0;
    return g_buckets[hdr->bucket_idx].block_size - sizeof(block_hdr_t);
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

    /*
     * A bucket block that still lands in the same bucket already has the room:
     * the allocation is rounded up to a power of two, so growing 40 bytes to 50
     * needs nothing but a new size in the header. Only the shrink direction was
     * taking that shortcut, which meant every grow inside one bucket paid for a
     * fresh block plus a memcpy plus a free — the common case for anything that
     * appends.
     *
     * Large allocations are excluded: their page count is recomputed from
     * hdr->size at kfree(), so growing the recorded size without growing the
     * mapping would free pages that were never allocated.
     */
    if (same_bucket && hdr->bucket_idx != 0xFF) {
        hdr->size = new_size;
        return ptr;
    }
    if (new_size <= old_size && same_bucket) {
        return ptr;
    }

    /*
     * Large allocation staying large and still covered by the same number of
     * pages: the physical pages are already mapped and contiguous, so only the
     * recorded size needs to move. kfree() recomputes the page count from
     * hdr->size, so this is safe as long as that count does not change — which
     * is exactly the condition checked here. Without this, appending to any
     * buffer above the largest bucket paid for a fresh multi-page allocation, a
     * full memcpy and a free on every growth step.
     */
    if (hdr->bucket_idx == 0xFF && size_to_bucket(new_size) == -1 &&
        new_size <= (1024ULL * 1024 * 1024)) {
        size_t old_pages = (old_size + sizeof(block_hdr_t) + PAGE_SIZE - 1) / PAGE_SIZE;
        size_t new_pages = (new_size + sizeof(block_hdr_t) + PAGE_SIZE - 1) / PAGE_SIZE;
        if (old_pages == new_pages) {
            hdr->size = new_size;
            return ptr;
        }
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
        __atomic_sub_fetch(&g_large_pages, pages, __ATOMIC_RELAXED);
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

/* ============================================================================
 * Empty-slab reclaim
 *
 * The bucket allocator only ever grows: a burst of small allocations pulls
 * pages from the PMM one at a time, and freeing the objects returns them to a
 * bucket free list, never to the PMM. A long-lived system that once had many
 * short-lived tasks (each fork/exec churns the heap) therefore keeps that peak
 * pinned forever.
 *
 * kmalloc_reclaim() walks each bucket's free list, and for every page whose
 * objects are *all* free it unlinks them and hands the page back. The free list
 * is address-sorted first so the objects of one page sit together and a single
 * linear pass can count them; nothing on the allocation fast path changes.
 * ========================================================================== */

#define KMALLOC_RECLAIM_MAX_PAGES 64            /* per bucket, per call */
#define KMALLOC_REAP_INTERVAL_TICKS 1000        /* ~10 s at 10 ms/tick */

static free_block_t *fb_merge(free_block_t *a, free_block_t *b)
{
    free_block_t dummy;
    free_block_t *t = &dummy;
    dummy.next = NULL;
    while (a && b) {
        if ((uintptr_t)a <= (uintptr_t)b) { t->next = a; a = a->next; }
        else                              { t->next = b; b = b->next; }
        t = t->next;
    }
    t->next = a ? a : b;
    return dummy.next;
}

static free_block_t *fb_sort(free_block_t *h)
{
    if (!h || !h->next) return h;
    free_block_t *slow = h, *fast = h->next;
    while (fast && fast->next) { slow = slow->next; fast = fast->next->next; }
    free_block_t *mid = slow->next;
    slow->next = NULL;
    return fb_merge(fb_sort(h), fb_sort(mid));
}

static size_t bucket_reclaim(bucket_t *b)
{
    size_t bpp = PAGE_SIZE / b->block_size;     /* objects per page, >= 1 */
    void  *pages[KMALLOC_RECLAIM_MAX_PAGES];
    size_t npages = 0;

    irqflags_t flags = spinlock_lock_irqsave(&b->lock);

    free_block_t *cur = fb_sort(b->free_list);
    free_block_t *newhead = NULL, *newtail = NULL;

    while (cur) {
        uintptr_t page = (uintptr_t)cur & ~(uintptr_t)(PAGE_SIZE - 1);
        free_block_t *runend = cur;
        size_t cnt = 1;
        while (runend->next &&
               ((uintptr_t)runend->next & ~(uintptr_t)(PAGE_SIZE - 1)) == page) {
            runend = runend->next;
            cnt++;
        }
        free_block_t *after = runend->next;

        if (cnt == bpp && npages < KMALLOC_RECLAIM_MAX_PAGES) {
            /* Whole page is free — drop the run, remember the page. */
            pages[npages++] = (void *)page;
        } else {
            /* Keep [cur .. runend] on the free list. */
            runend->next = NULL;
            if (newtail) newtail->next = cur;
            else         newhead = cur;
            newtail = runend;
        }
        cur = after;
    }
    b->free_list = newhead;
    b->pages -= npages;

    spinlock_unlock_irqrestore(&b->lock, flags);

    for (size_t i = 0; i < npages; i++)
        pmm_free_page(VIRT_TO_PHYS((virt_addr_t)pages[i]));

    return npages;
}

void kmalloc_meminfo(u64 *slab_kb, u64 *large_kb)
{
    u64 per_page_kb = PAGE_SIZE / 1024;
    if (slab_kb) {
        u64 sp = 0;
        for (int i = 0; i < BUCKET_COUNT; i++) sp += g_buckets[i].pages;
        *slab_kb = sp * per_page_kb;
    }
    if (large_kb)
        *large_kb = __atomic_load_n(&g_large_pages, __ATOMIC_RELAXED) * per_page_kb;
}

int kmalloc_slab_stats(kmalloc_slab_stat_t *out, int max)
{
    int n = BUCKET_COUNT < max ? BUCKET_COUNT : max;
    for (int i = 0; i < n; i++) {
        bucket_t *b = &g_buckets[i];
        u32 obj = (u32)(b->block_size - sizeof(block_hdr_t));
        u32 per = (u32)(PAGE_SIZE / b->block_size);

        irqflags_t flags = spinlock_lock_irqsave(&b->lock);
        u64 pages = b->pages;
        u64 freen = 0;
        for (free_block_t *f = b->free_list; f; f = f->next) freen++;
        spinlock_unlock_irqrestore(&b->lock, flags);

        u64 total = pages * per;
        out[i].obj_size      = obj;
        out[i].objs_per_slab = per;
        out[i].pages         = pages;
        out[i].total_objs    = total;
        out[i].active_objs   = total > freen ? total - freen : 0;
    }
    return n;
}

size_t kmalloc_reclaim(void)
{
    size_t freed = 0;
    for (int i = 0; i < BUCKET_COUNT; i++)
        freed += bucket_reclaim(&g_buckets[i]);
    if (freed)
        pr_debug("[KMALLOC] reclaim: %lu slab page(s) returned to PMM\n",
                 (unsigned long)freed);
    return freed;
}

static void kmalloc_reaper_thread(void *arg)
{
    (void)arg;
    for (;;) {
        sched_sleep(KMALLOC_REAP_INTERVAL_TICKS);
        kmalloc_reclaim();
    }
}

void kmalloc_start_reaper(void)
{
    process_t *kproc = sched_kernel_process();
    if (kproc)
        thread_create(kproc, (uintptr_t)kmalloc_reaper_thread, 0, true);
}
