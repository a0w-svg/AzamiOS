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
#include "../../arch/x86_64/cpu/hwaccel.h"
#include "../../arch/x86_64/cpu/smp.h"
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

    /* Smallest bucket i (block_size == 1 << (i + MIN_BUCKET_SHIFT)) with
     * block_size >= total — same ceil(log2) construction as pmm.c's
     * pages_to_order(), replacing a linear scan through up to BUCKET_COUNT
     * buckets with one LZCNT. This runs on the front of every kmalloc(),
     * ahead of even the per-CPU magazine fast path above. */
    u32 shift = (total <= 1) ? 0 : (u32)(64 - hw_clz64((u64)total - 1));
    if (shift <= MIN_BUCKET_SHIFT) return 0;
    u32 idx = shift - MIN_BUCKET_SHIFT;
    return (idx < BUCKET_COUNT) ? (int)idx : -1;
}

/* ============================================================================
 * Per-CPU magazines
 *
 * Every bucket alloc/free above serialises on one of BUCKET_COUNT global
 * spinlocks. On a multi-core build that is the kernel's busiest lock, and the
 * cache line under it ping-pongs between cores on traffic that is otherwise
 * embarrassingly parallel. A magazine is a small per-CPU stack of ready
 * objects for one size class: the fast path pops or pushes it with interrupts
 * merely disabled — no lock, no shared cache line — and only touches the
 * bucket when its magazine runs dry (one bulk refill) or overflows (one bulk
 * flush). Objects are identical wherever they were carved, so the CPU that
 * frees one need not be the CPU that allocated it.
 *
 * A magazine is only ever mutated on its owning CPU with interrupts off, which
 * is what makes the lockless path safe: interrupt-context allocations on the
 * same core are excluded, and no other core touches the row. Nothing here
 * drains another CPU's magazine — kmalloc_slab_stats() and kmalloc_reclaim()
 * read the counts racily and treat what they see as an estimate, exactly as
 * the page counters above already do.
 *
 * Disabled until kmalloc_enable_percpu(), which the boot path calls once
 * smp_init() has published a valid GS base on every core; before that the BSP
 * is single-threaded and allocates straight from the shared buckets.
 * ========================================================================== */

#define MAG_CAP     62                  /* objects cached per CPU per bucket   */
#define MAG_BATCH   31                  /* moved in/out on a miss / overflow   */

typedef struct {
    u32   count;
    void *slot[MAG_CAP];
} magazine_t;

/* [cpu][bucket]. ~250 KiB of BSS at the 64-core ceiling (SMP_MAX_CPUS *
 * BUCKET_COUNT * sizeof(magazine_t)); each core writes only its own row, and
 * the table is cache-line aligned. */
static magazine_t g_mag[SMP_MAX_CPUS][BUCKET_COUNT]
    __attribute__((aligned(64)));

static bool g_percpu_ready = false;

static __always_inline irqflags_t irq_save(void)
{
    irqflags_t f;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(f) : : "memory");
    return f;
}
static __always_inline void irq_restore(irqflags_t f)
{
    __asm__ volatile("pushq %0; popfq" : : "r"(f) : "memory");
}

/*
 * Pop up to @want raw objects (block_hdr region) from bucket @b's shared free
 * list into @out, refilling one page from the PMM if the list is empty. One
 * lock acquisition covers the whole batch. Returns how many were obtained;
 * fewer than @want (possibly 0) means the PMM is out of memory.
 */
static size_t bucket_bulk_alloc(bucket_t *b, void **out, size_t want)
{
    size_t got = 0;
    irqflags_t flags = spinlock_lock_irqsave(&b->lock);

    while (got < want) {
        if (!b->free_list) {
            /* Same as the historical single-object path: drop the bucket lock
             * across pmm_alloc_page() so the bucket->pmm lock order is never
             * held both ways, build the slice list locally, splice it back. */
            spinlock_unlock_irqrestore(&b->lock, flags);

            phys_addr_t page = pmm_alloc_page();
            if (!page) return got;

            u8 *virt = (u8 *)PHYS_TO_VIRT(page);
            size_t blk_size = b->block_size;
            size_t count = PAGE_SIZE / blk_size;

            free_block_t *local_head = NULL;
            for (size_t j = 0; j < count; j++) {
                free_block_t *blk = (free_block_t *)(virt + j * blk_size);
                blk->next = local_head;
                local_head = blk;
            }

            flags = spinlock_lock_irqsave(&b->lock);
            free_block_t *tail = local_head;
            while (tail->next) tail = tail->next;
            tail->next = b->free_list;
            b->free_list = local_head;
            b->pages++;
        }

        free_block_t *blk = b->free_list;
        b->free_list = blk->next;
        out[got++] = blk;
    }

    spinlock_unlock_irqrestore(&b->lock, flags);
    return got;
}

/* Push @n objects back onto bucket @b's shared free list under one lock. */
static void bucket_bulk_free(bucket_t *b, void **objs, size_t n)
{
    if (n == 0) return;

    for (size_t i = 0; i + 1 < n; i++)
        ((free_block_t *)objs[i])->next = (free_block_t *)objs[i + 1];

    irqflags_t flags = spinlock_lock_irqsave(&b->lock);
    ((free_block_t *)objs[n - 1])->next = b->free_list;
    b->free_list = (free_block_t *)objs[0];
    spinlock_unlock_irqrestore(&b->lock, flags);
}

/* Fast-path bucket allocation: try this CPU's magazine, refill it from the
 * shared list on a miss, fall back to a direct shared-list pop. Returns a raw
 * object pointer (caller writes the header) or NULL on OOM. */
static void *bucket_alloc(int idx)
{
    bucket_t *b = &g_buckets[idx];
    void *obj = NULL;

    if (g_percpu_ready) {
        irqflags_t f = irq_save();
        magazine_t *m = &g_mag[smp_current_cpu_id()][idx];
        if (m->count == 0)
            m->count = (u32)bucket_bulk_alloc(b, m->slot, MAG_BATCH);
        if (m->count > 0)
            obj = m->slot[--m->count];
        irq_restore(f);
    }

    if (!obj) {
        void *one[1];
        if (bucket_bulk_alloc(b, one, 1) == 1)
            obj = one[0];
    }
    return obj;
}

/* Fast-path bucket free: push onto this CPU's magazine, flushing a batch back
 * to the shared list first if it is full. Returns false if the magazine layer
 * is not active yet, in which case the caller uses the shared list directly. */
static bool bucket_free(int idx, void *obj)
{
    if (!g_percpu_ready) return false;

    bucket_t *b = &g_buckets[idx];
    irqflags_t f = irq_save();
    magazine_t *m = &g_mag[smp_current_cpu_id()][idx];
    if (m->count == MAG_CAP) {
        bucket_bulk_free(b, &m->slot[MAG_BATCH], MAG_CAP - MAG_BATCH);
        m->count = MAG_BATCH;
    }
    m->slot[m->count++] = obj;
    irq_restore(f);
    return true;
}

/* Best-effort count of objects parked in every CPU's magazine for bucket @idx.
 * Raced against live pushes/pops on other cores by design; the callers that
 * use it (slabinfo, reclaim) only ever want an estimate. */
static u64 magazine_parked(int idx)
{
    u64 n = 0;
    u32 ncpu = g_percpu_ready ? smp_cpu_count() : 0;
    if (ncpu > SMP_MAX_CPUS) ncpu = SMP_MAX_CPUS;
    for (u32 c = 0; c < ncpu; c++)
        n += __atomic_load_n(&g_mag[c][idx].count, __ATOMIC_RELAXED);
    return n;
}

void kmalloc_enable_percpu(void)
{
    g_percpu_ready = true;
    pr_debug("[KMALLOC] per-CPU magazines active (cap %d, batch %d)\n",
             MAG_CAP, MAG_BATCH);
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

    void *raw = bucket_alloc(idx);
    if (!raw) return NULL;

    block_hdr_t *hdr = (block_hdr_t *)raw;
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

    /* Fast path: back onto this CPU's magazine. Falls through to the shared
     * free list when the magazine layer is not enabled yet (early boot). */
    if (bucket_free((int)idx, hdr))
        return;

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

        /* Objects parked in per-CPU magazines are free too, just not on the
         * shared list — without this they would show up as "active". */
        freen += magazine_parked(i);

        u64 total = pages * per;
        out[i].obj_size      = obj;
        out[i].objs_per_slab = per;
        out[i].pages         = pages;
        out[i].total_objs    = total;
        out[i].active_objs   = total > freen ? total - freen : 0;
    }
    return n;
}

/*
 * Return this CPU's magazines to the shared free lists. Runs with interrupts
 * off on the calling core only — draining a remote core's magazine would race
 * its lockless fast path — so a caller that wants every core drained has to
 * arrange to run this on each. The reaper does it once per pass and migrates
 * over time; a fully-free page pinned by a stray cached object elsewhere is
 * simply reclaimed on a later tick.
 */
void kmalloc_drain_local(void)
{
    if (!g_percpu_ready) return;

    for (int i = 0; i < BUCKET_COUNT; i++) {
        void *tmp[MAG_CAP];
        size_t n;

        irqflags_t f = irq_save();
        magazine_t *m = &g_mag[smp_current_cpu_id()][i];
        n = m->count;
        for (size_t j = 0; j < n; j++) tmp[j] = m->slot[j];
        m->count = 0;
        irq_restore(f);

        bucket_bulk_free(&g_buckets[i], tmp, n);
    }
}

size_t kmalloc_reclaim(void)
{
    kmalloc_drain_local();

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
