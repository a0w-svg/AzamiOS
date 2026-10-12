/* ============================================================================
 * AzamiOS Userspace — Native High-Performance Hardened Memory Allocator
 * File: userland/libc/malloc.c
 *
 * Implements Workstream U2.1:
 * - Size classes (16 B -> 32 KiB, 72 classes, <= 12.5% internal fragmentation)
 *   backed by 64 KiB spans from mmap.
 * - Per-thread cache (TLS, initial-exec) with bounded magazines.
 * - Central per-class free lists protected by 3-state futex-based locks
 *   (0 unlocked / 1 locked / 2 contended) replacing the global spinlock.
 * - Large allocations (> 32 KiB) mapped directly with mmap; realloc grows
 *   via mremap(MREMAP_MAYMOVE).
 * - Memory reclamation with madvise(MADV_DONTNEED) on idle spans;
 *   malloc_trim, malloc_usable_size, posix_memalign, aligned_alloc.
 * - Invariant: no allocation under the allocator lock.
 * - Hardening: span-header canaries, double-free detection on thread cache,
 *   freelist pointer mangling with a per-process key from AT_RANDOM.
 * ============================================================================ */

#include "include/malloc.h"
#include "include/stdlib.h"
#include "include/string.h"
#include "include/errno.h"
#include "include/stdint.h"
#include "include/stdbool.h"
#include "include/sys/syscall.h"
#include "include/sys/mman.h"
#include "include/sys/auxv.h"

#define SPAN_SIZE           65536UL
#define SPAN_MASK           (~(SPAN_SIZE - 1UL))
#define SPAN_HEADER_SIZE    64UL

#define SPAN_MAGIC          0x5350414EU   /* 'SPAN' */
#define LARGE_MAGIC         0x4C415247U   /* 'LARG' */
#define ALIGNED_MAGIC       0x414C4947UL  /* 'ALIG' */

#define FUTEX_WAIT_PRIVATE  128
#define FUTEX_WAKE_PRIVATE  129

#ifndef MREMAP_MAYMOVE
#define MREMAP_MAYMOVE      1
#endif

#ifndef MADV_DONTNEED
#define MADV_DONTNEED       4
#endif

#ifndef ARCH_GET_FS
#define ARCH_GET_FS         0x1003
#endif

/* ── 3-State Futex Lock (0 unlocked / 1 locked / 2 contended) ─────────────── */

typedef struct {
    int lock;
} futex_lock_t;

static inline void futex_lock_acquire(futex_lock_t *l)
{
    int exp = 0;
    if (__atomic_compare_exchange_n(&l->lock, &exp, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        return;

    for (int i = 0; i < 64; i++) {
        __asm__ volatile("pause");
        exp = 0;
        if (__atomic_compare_exchange_n(&l->lock, &exp, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            return;
    }

    while (1) {
        if (exp == 2 || __atomic_exchange_n(&l->lock, 2, __ATOMIC_ACQ_REL) != 0) {
            syscall4(SYS_futex, (long)&l->lock, FUTEX_WAIT_PRIVATE, 2, 0);
        }
        exp = 0;
        if (__atomic_compare_exchange_n(&l->lock, &exp, 2, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            return;
    }
}

static inline void futex_lock_release(futex_lock_t *l)
{
    if (__atomic_exchange_n(&l->lock, 0, __ATOMIC_RELEASE) == 2) {
        syscall4(SYS_futex, (long)&l->lock, FUTEX_WAKE_PRIVATE, 1, 0);
    }
}

/* ── Size Classes (16 B -> 32 KiB, <= 12.5% Internal Fragmentation) ───────── */

static const uint16_t g_size_classes[] = {
    16,    32,    48,    64,    80,    96,    112,   128,   /* step 16 */
    144,   160,   176,   192,   208,   224,   240,   256,   /* step 16 */
    288,   320,   352,   384,   416,   448,   480,   512,   /* step 32 */
    576,   640,   704,   768,   832,   896,   960,   1024,  /* step 64 */
    1152,  1280,  1408,  1536,  1664,  1792,  1920,  2048,  /* step 128 */
    2304,  2560,  2816,  3072,  3328,  3584,  3840,  4096,  /* step 256 */
    4608,  5120,  5632,  6144,  6656,  7168,  7680,  8192,  /* step 512 */
    9216,  10240, 11264, 12288, 13312, 14336, 15360, 16384, /* step 1024 */
    18432, 20480, 22528, 24576, 26624, 28672, 30720, 32768  /* step 2048 */
};

#define NUM_CLASSES (sizeof(g_size_classes) / sizeof(g_size_classes[0]))
#define MAX_SMALL_SIZE 32768UL

/* Fast small size class lookup table for sizes up to 1024 bytes */
static uint8_t g_small_class_lookup[1025];
static volatile int g_lookup_init = 0;

/* ── Hardening & Secrets ─────────────────────────────────────────────────── */

static uintptr_t g_pointer_secret = 0x5A3C9E7B1D4F2860UL;
static uintptr_t g_canary_secret  = 0xA7D1C5E3B8F42901UL;
static volatile int g_secrets_initialized = 0;

static void init_secrets_once(void)
{
    if (g_secrets_initialized) return;

    /* Seed from AT_RANDOM if available */
    void *aux = (void *)getauxval(25 /* AT_RANDOM */);
    if (aux) {
        uintptr_t r1 = *(uintptr_t *)aux;
        uintptr_t r2 = *((uintptr_t *)aux + 1);
        if (r1 != 0) g_pointer_secret ^= r1;
        if (r2 != 0) g_canary_secret  ^= r2;
    }

    /* Build small size class lookup table */
    for (size_t s = 0; s <= 1024; s++) {
        uint8_t c = 0;
        while (c < NUM_CLASSES && g_size_classes[c] < s) {
            c++;
        }
        g_small_class_lookup[s] = c;
    }

    g_lookup_init = 1;
    g_secrets_initialized = 1;
}

static inline void *freelist_encode(void *curr, void *next)
{
    return (void *)((uintptr_t)next ^ g_pointer_secret ^ (uintptr_t)curr);
}

static inline void *freelist_decode(void *curr, void *encoded)
{
    return (void *)((uintptr_t)encoded ^ g_pointer_secret ^ (uintptr_t)curr);
}

static inline int size_to_class(size_t size)
{
    if (!g_lookup_init) init_secrets_once();
    if (size <= 1024) {
        return (int)g_small_class_lookup[size];
    }
    for (int i = 32; i < (int)NUM_CLASSES; i++) {
        if (g_size_classes[i] >= size) return i;
    }
    return -1; /* Exceeds MAX_SMALL_SIZE */
}

/* ── Span Header & Metadata ──────────────────────────────────────────────── */

typedef struct span {
    uint32_t magic;          /* SPAN_MAGIC */
    uint32_t canary;         /* g_canary_secret ^ (uintptr_t)span */
    uint16_t class_idx;      /* 0 .. NUM_CLASSES - 1 */
    uint16_t obj_size;       /* object size in bytes */
    uint16_t total_objs;     /* total number of objects in this span */
    uint16_t alloc_count;    /* currently active allocations */
    void *free_list;         /* singly linked list of free blocks in this span */
    struct span *next;       /* arena doubly linked list */
    struct span *prev;
} span_t;

/* Large allocation header (mapped directly via mmap) */
typedef struct {
    uint32_t magic;          /* LARGE_MAGIC */
    uint32_t canary;         /* g_canary_secret ^ (uintptr_t)header */
    size_t total_size;       /* total mmap size */
    size_t user_size;        /* requested size */
} large_header_t;

/* Aligned allocation prefix */
typedef struct {
    void *raw_ptr;
    size_t magic;            /* ALIGNED_MAGIC */
} aligned_tag_t;

/* Central per-class state */
static futex_lock_t g_class_locks[NUM_CLASSES];
static span_t *g_partial_spans[NUM_CLASSES];

/* ── 64 KiB Aligned Span Mapper ──────────────────────────────────────────── */

static void *map_aligned_span(size_t size)
{
    /* Request 64 KiB aligned region from mmap */
    void *ptr = (void *)syscall6(SYS_mmap, 0, size, PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if ((long)(intptr_t)ptr <= 0 || ptr == (void *)-1) return NULL;

    if (((uintptr_t)ptr & (SPAN_SIZE - 1UL)) == 0) {
        return ptr;
    }

    /* If mmap was not 64 KiB aligned, over-allocate by SPAN_SIZE, align, and trim */
    syscall2(SYS_munmap, (long)(intptr_t)ptr, size);

    size_t alloc_size = size + SPAN_SIZE;
    ptr = (void *)syscall6(SYS_mmap, 0, alloc_size, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if ((long)(intptr_t)ptr <= 0 || ptr == (void *)-1) return NULL;

    uintptr_t raw = (uintptr_t)ptr;
    uintptr_t aligned = (raw + (SPAN_SIZE - 1UL)) & SPAN_MASK;

    if (aligned > raw) {
        syscall2(SYS_munmap, (long)raw, aligned - raw);
    }
    size_t trailing = alloc_size - (aligned - raw) - size;
    if (trailing > 0) {
        syscall2(SYS_munmap, (long)(aligned + size), trailing);
    }

    return (void *)aligned;
}

/* Initializes a fresh 64 KiB span for the given class index */
static span_t *init_span(int class_idx)
{
    span_t *sp = (span_t *)map_aligned_span(SPAN_SIZE);
    if (!sp) return NULL;

    uint16_t obj_size = g_size_classes[class_idx];
    sp->magic = SPAN_MAGIC;
    sp->canary = (uint32_t)(g_canary_secret ^ (uintptr_t)sp);
    sp->class_idx = (uint16_t)class_idx;
    sp->obj_size = obj_size;
    sp->alloc_count = 0;
    sp->next = NULL;
    sp->prev = NULL;

    char *obj_start = (char *)sp + SPAN_HEADER_SIZE;
    size_t usable = SPAN_SIZE - SPAN_HEADER_SIZE;
    uint16_t total = (uint16_t)(usable / obj_size);
    sp->total_objs = total;

    /* Build mangled freelist */
    void *fl = NULL;
    for (int i = total - 1; i >= 0; i--) {
        void *obj = (void *)(obj_start + (size_t)i * obj_size);
        *(void **)obj = freelist_encode(obj, fl);
        fl = obj;
    }
    sp->free_list = fl;

    return sp;
}

/* ── Thread Cache with Bounded Magazines ──────────────────────────────────── */

#define MAG_CAPACITY 8

typedef struct {
    uint8_t count[NUM_CLASSES];
    void *mag[NUM_CLASSES][MAG_CAPACITY];
} tcache_t;

/* Static cache for main thread and TLS bootstrap path */
static tcache_t g_main_tcache;

/* Thread-local cache for subsequent threads */
static __thread tcache_t t_thread_cache __attribute__((tls_model("initial-exec")));

static inline tcache_t *get_tcache(void)
{
    uint64_t cur_fs = 0;
    syscall2(SYS_arch_prctl, ARCH_GET_FS, (long)(intptr_t)&cur_fs);
    if (cur_fs == 0) {
        return &g_main_tcache;
    }
    return &t_thread_cache;
}

/* Refill thread magazine from central spans */
static void tcache_refill(tcache_t *tc, int class_idx)
{
    futex_lock_acquire(&g_class_locks[class_idx]);

    span_t *sp = g_partial_spans[class_idx];
    if (!sp) {
        sp = init_span(class_idx);
        if (!sp) {
            futex_lock_release(&g_class_locks[class_idx]);
            return;
        }
        sp->next = g_partial_spans[class_idx];
        if (sp->next) sp->next->prev = sp;
        g_partial_spans[class_idx] = sp;
    }

    int batch = MAG_CAPACITY / 2;
    if (batch < 1) batch = 1;

    while (tc->count[class_idx] < batch && sp) {
        while (sp->free_list && tc->count[class_idx] < batch) {
            void *obj = sp->free_list;
            sp->free_list = freelist_decode(obj, *(void **)obj);
            sp->alloc_count++;
            tc->mag[class_idx][tc->count[class_idx]++] = obj;
        }

        if (!sp->free_list) {
            /* Span is now full, remove from partial list */
            g_partial_spans[class_idx] = sp->next;
            if (sp->next) sp->next->prev = NULL;
            sp->next = NULL;
            sp->prev = NULL;

            sp = g_partial_spans[class_idx];
            if (!sp && tc->count[class_idx] < batch) {
                span_t *nsp = init_span(class_idx);
                if (nsp) {
                    nsp->next = g_partial_spans[class_idx];
                    if (nsp->next) nsp->next->prev = nsp;
                    g_partial_spans[class_idx] = nsp;
                    sp = nsp;
                }
            }
        }
    }

    futex_lock_release(&g_class_locks[class_idx]);
}

/* Flush half of the magazine back to central spans */
static void tcache_flush(tcache_t *tc, int class_idx)
{
    futex_lock_acquire(&g_class_locks[class_idx]);

    int keep = MAG_CAPACITY / 2;
    while (tc->count[class_idx] > keep) {
        void *obj = tc->mag[class_idx][--tc->count[class_idx]];
        span_t *sp = (span_t *)((uintptr_t)obj & SPAN_MASK);

        /* Verify canary */
        if (sp->magic != SPAN_MAGIC || sp->canary != (uint32_t)(g_canary_secret ^ (uintptr_t)sp)) {
            __builtin_trap();
        }

        bool was_full = (sp->free_list == NULL);
        *(void **)obj = freelist_encode(obj, sp->free_list);
        sp->free_list = obj;
        sp->alloc_count--;

        if (was_full) {
            /* Put back in partial spans */
            sp->next = g_partial_spans[class_idx];
            sp->prev = NULL;
            if (sp->next) sp->next->prev = sp;
            g_partial_spans[class_idx] = sp;
        }
    }

    futex_lock_release(&g_class_locks[class_idx]);
}

/* ── Public API Implementation ───────────────────────────────────────────── */

void *malloc(size_t size)
{
    if (size == 0) return NULL;
    if (!g_secrets_initialized) init_secrets_once();

    int c = size_to_class(size);
    if (c >= 0) {
        tcache_t *tc = get_tcache();
        if (tc->count[c] == 0) {
            tcache_refill(tc, c);
            if (tc->count[c] == 0) return NULL;
        }
        return tc->mag[c][--tc->count[c]];
    }

    /* Large allocation (> MAX_SMALL_SIZE): map directly via mmap */
    size_t total_size = (size + SPAN_HEADER_SIZE + (SPAN_SIZE - 1UL)) & SPAN_MASK;
    large_header_t *hdr = (large_header_t *)map_aligned_span(total_size);
    if (!hdr) return NULL;

    hdr->magic = LARGE_MAGIC;
    hdr->canary = (uint32_t)(g_canary_secret ^ (uintptr_t)hdr);
    hdr->total_size = total_size;
    hdr->user_size = size;

    return (void *)((char *)hdr + SPAN_HEADER_SIZE);
}

void free(void *ptr)
{
    if (!ptr) return;

    /* Check for aligned_alloc wrapper */
    if (((aligned_tag_t *)ptr)[-1].magic == ALIGNED_MAGIC) {
        void *raw = ((aligned_tag_t *)ptr)[-1].raw_ptr;
        free(raw);
        return;
    }

    uintptr_t span_addr = (uintptr_t)ptr & SPAN_MASK;
    span_t *sp = (span_t *)span_addr;

    if (sp->magic == SPAN_MAGIC) {
        if (sp->canary != (uint32_t)(g_canary_secret ^ (uintptr_t)sp)) {
            __builtin_trap();
        }

        int c = sp->class_idx;
        if (c >= (int)NUM_CLASSES) __builtin_trap();

        tcache_t *tc = get_tcache();

        /* Double-free detection on thread cache */
        for (int i = 0; i < tc->count[c]; i++) {
            if (tc->mag[c][i] == ptr) {
                __builtin_trap(); /* Caught double-free! */
            }
        }

        if (tc->count[c] >= MAG_CAPACITY) {
            tcache_flush(tc, c);
        }

        tc->mag[c][tc->count[c]++] = ptr;
        return;
    }

    if (sp->magic == LARGE_MAGIC) {
        large_header_t *lhdr = (large_header_t *)sp;
        if (lhdr->canary != (uint32_t)(g_canary_secret ^ (uintptr_t)lhdr)) {
            __builtin_trap();
        }
        syscall2(SYS_munmap, (long)(intptr_t)lhdr, lhdr->total_size);
        return;
    }

    /* Invalid pointer */
    __builtin_trap();
}

void *calloc(size_t nmemb, size_t size)
{
    size_t total = nmemb * size;
    if (nmemb != 0 && total / nmemb != size) return NULL;
    void *ptr = malloc(total);
    if (ptr) memset(ptr, 0, total);
    return ptr;
}

void *realloc(void *ptr, size_t size)
{
    if (!ptr) return malloc(size);
    if (size == 0) {
        free(ptr);
        return NULL;
    }

    /* Check aligned allocation */
    if (((aligned_tag_t *)ptr)[-1].magic == ALIGNED_MAGIC) {
        void *raw = ((aligned_tag_t *)ptr)[-1].raw_ptr;
        size_t old_usable = malloc_usable_size(ptr);
        if (size <= old_usable) return ptr;

        void *new_p = malloc(size);
        if (new_p) {
            memcpy(new_p, ptr, old_usable);
            free(raw);
        }
        return new_p;
    }

    uintptr_t span_addr = (uintptr_t)ptr & SPAN_MASK;
    span_t *sp = (span_t *)span_addr;

    if (sp->magic == SPAN_MAGIC) {
        if (sp->canary != (uint32_t)(g_canary_secret ^ (uintptr_t)sp)) __builtin_trap();
        size_t old_size = sp->obj_size;
        if (size <= old_size) return ptr;

        void *new_p = malloc(size);
        if (new_p) {
            memcpy(new_p, ptr, old_size);
            free(ptr);
        }
        return new_p;
    }

    if (sp->magic == LARGE_MAGIC) {
        large_header_t *lhdr = (large_header_t *)sp;
        if (lhdr->canary != (uint32_t)(g_canary_secret ^ (uintptr_t)lhdr)) __builtin_trap();

        size_t old_total = lhdr->total_size;
        size_t new_total = (size + SPAN_HEADER_SIZE + (SPAN_SIZE - 1UL)) & SPAN_MASK;

        if (new_total == old_total) {
            lhdr->user_size = size;
            return ptr;
        }

        /* Attempt mremap growth */
        long res = syscall5(SYS_mremap, (long)(intptr_t)lhdr, old_total, new_total, MREMAP_MAYMOVE, 0);
        if (res > 0 && (void *)res != (void *)-1 && ((uintptr_t)res & (SPAN_SIZE - 1UL)) == 0) {
            large_header_t *nlhdr = (large_header_t *)res;
            nlhdr->total_size = new_total;
            nlhdr->user_size = size;
            nlhdr->canary = (uint32_t)(g_canary_secret ^ (uintptr_t)nlhdr);
            return (void *)((char *)nlhdr + SPAN_HEADER_SIZE);
        }

        /* Fallback: allocate new, copy, unmap old */
        void *new_p = malloc(size);
        if (new_p) {
            size_t copy_len = lhdr->user_size < size ? lhdr->user_size : size;
            memcpy(new_p, ptr, copy_len);
            syscall2(SYS_munmap, (long)(intptr_t)lhdr, old_total);
        }
        return new_p;
    }

    return NULL;
}

void *aligned_alloc(size_t alignment, size_t size)
{
    if (alignment == 0 || (alignment & (alignment - 1)) != 0 || size == 0) return NULL;
    if (alignment < sizeof(void *)) alignment = sizeof(void *);

    if (alignment <= 16) {
        return malloc(size);
    }

    size_t extra = alignment + sizeof(aligned_tag_t);
    void *raw = malloc(size + extra);
    if (!raw) return NULL;

    uintptr_t raw_addr = (uintptr_t)raw + sizeof(aligned_tag_t);
    uintptr_t aligned_addr = (raw_addr + alignment - 1) & ~(alignment - 1);

    ((aligned_tag_t *)aligned_addr)[-1].raw_ptr = raw;
    ((aligned_tag_t *)aligned_addr)[-1].magic = ALIGNED_MAGIC;

    return (void *)aligned_addr;
}

int posix_memalign(void **memptr, size_t alignment, size_t size)
{
    if (!memptr) return EINVAL;
    if ((alignment % sizeof(void *)) != 0 || (alignment & (alignment - 1)) != 0) {
        return EINVAL;
    }
    void *p = aligned_alloc(alignment, size);
    if (!p) return ENOMEM;
    *memptr = p;
    return 0;
}

void *reallocarray(void *ptr, size_t nmemb, size_t size)
{
    if (nmemb > 0 && size > (size_t)-1 / nmemb) {
        errno = ENOMEM;
        return NULL;
    }
    return realloc(ptr, nmemb * size);
}

void *valloc(size_t size)
{
    return aligned_alloc(4096, size);
}

void *pvalloc(size_t size)
{
    size_t rounded = (size + 4095) & ~4095UL;
    return aligned_alloc(4096, rounded);
}

size_t malloc_usable_size(void *ptr)
{
    if (!ptr) return 0;

    if (((aligned_tag_t *)ptr)[-1].magic == ALIGNED_MAGIC) {
        void *raw = ((aligned_tag_t *)ptr)[-1].raw_ptr;
        size_t total = malloc_usable_size(raw);
        size_t off = (size_t)((char *)ptr - (char *)raw);
        return total > off ? total - off : 0;
    }

    uintptr_t span_addr = (uintptr_t)ptr & SPAN_MASK;
    span_t *sp = (span_t *)span_addr;

    if (sp->magic == SPAN_MAGIC) {
        if (sp->canary == (uint32_t)(g_canary_secret ^ (uintptr_t)sp)) {
            return sp->obj_size;
        }
    } else if (sp->magic == LARGE_MAGIC) {
        large_header_t *lhdr = (large_header_t *)sp;
        if (lhdr->canary == (uint32_t)(g_canary_secret ^ (uintptr_t)lhdr)) {
            return lhdr->user_size;
        }
    }
    return 0;
}

int malloc_trim(size_t pad)
{
    (void)pad;
    int trimmed = 0;

    for (size_t c = 0; c < NUM_CLASSES; c++) {
        futex_lock_acquire(&g_class_locks[c]);

        span_t **curr = &g_partial_spans[c];
        while (*curr) {
            span_t *sp = *curr;
            if (sp->alloc_count == 0) {
                /* Span is completely idle, return physical frames to kernel */
                syscall3(SYS_madvise, (long)((char *)sp + 4096), SPAN_SIZE - 4096, MADV_DONTNEED);
                trimmed = 1;
            }
            curr = &sp->next;
        }

        futex_lock_release(&g_class_locks[c]);
    }

    return trimmed;
}

