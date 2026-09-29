#include <kernel/mm/dma.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/kmalloc.h>
#include <kernel/lib/string.h>
#include <azami/debug.h>
#include <arch/x86_64/cpu/spinlock.h>


#define MAX_DMA_MASKS 256
static spinlock_t dma_masks_lock = SPINLOCK_INIT;

struct dma_device_mask {
    device_t *dev;
    u64 dma_mask;
    u64 coherent_dma_mask;
};

static struct dma_device_mask device_masks[MAX_DMA_MASKS];
static int num_device_masks = 0;

static u64 get_dma_mask(device_t *dev) {
    u64 mask = DMA_BIT_MASK(64); // Default to 64-bit
    irqflags_t flags;
    flags = spinlock_lock_irqsave(&dma_masks_lock);
    for (int i = 0; i < num_device_masks; i++) {
        if (device_masks[i].dev == dev) {
            mask = device_masks[i].dma_mask;
            break;
        }
    }
    spinlock_unlock_irqrestore(&dma_masks_lock, flags);
    return mask;
}

static u64 get_coherent_dma_mask(device_t *dev) {
    u64 mask = DMA_BIT_MASK(64);
    irqflags_t flags;
    flags = spinlock_lock_irqsave(&dma_masks_lock);
    for (int i = 0; i < num_device_masks; i++) {
        if (device_masks[i].dev == dev) {
            mask = device_masks[i].coherent_dma_mask;
            break;
        }
    }
    spinlock_unlock_irqrestore(&dma_masks_lock, flags);
    return mask;
}

bool dma_set_mask(device_t *dev, u64 mask) {
    irqflags_t flags;
    flags = spinlock_lock_irqsave(&dma_masks_lock);
    for (int i = 0; i < num_device_masks; i++) {
        if (device_masks[i].dev == dev) {
            device_masks[i].dma_mask = mask;
            spinlock_unlock_irqrestore(&dma_masks_lock, flags);
            return true;
        }
    }
    if (num_device_masks < MAX_DMA_MASKS) {
        device_masks[num_device_masks].dev = dev;
        device_masks[num_device_masks].dma_mask = mask;
        device_masks[num_device_masks].coherent_dma_mask = mask; // Default fallback
        num_device_masks++;
        spinlock_unlock_irqrestore(&dma_masks_lock, flags);
        return true;
    }
    spinlock_unlock_irqrestore(&dma_masks_lock, flags);
    return false;
}

bool dma_set_coherent_mask(device_t *dev, u64 mask) {
    irqflags_t flags;
    flags = spinlock_lock_irqsave(&dma_masks_lock);
    for (int i = 0; i < num_device_masks; i++) {
        if (device_masks[i].dev == dev) {
            device_masks[i].coherent_dma_mask = mask;
            spinlock_unlock_irqrestore(&dma_masks_lock, flags);
            return true;
        }
    }
    if (num_device_masks < MAX_DMA_MASKS) {
        device_masks[num_device_masks].dev = dev;
        device_masks[num_device_masks].dma_mask = mask;
        device_masks[num_device_masks].coherent_dma_mask = mask;
        num_device_masks++;
        spinlock_unlock_irqrestore(&dma_masks_lock, flags);
        return true;
    }
    spinlock_unlock_irqrestore(&dma_masks_lock, flags);
    return false;
}

static inline int get_order(size_t size) {
    int order = 0;
    size_t s = PAGE_SIZE;
    while (s < size) {
        order++;
        s <<= 1;
    }
    return order;
}

void *dma_alloc_coherent(device_t *dev, size_t size, dma_addr_t *dma_handle) {
    int order = get_order(size);
    phys_addr_t paddr = 0;
    u64 mask = get_coherent_dma_mask(dev);

    if (mask < DMA_BIT_MASK(64)) {
        paddr = pmm_alloc_32(order);
    } else {
        paddr = pmm_alloc(order);
    }

    if (!paddr)
        return NULL;

    *dma_handle = paddr;
    void *vaddr = (void *)PHYS_TO_VIRT(paddr);
    memset(vaddr, 0, size);
    return vaddr;
}

void dma_free_coherent(device_t *dev, size_t size, void *cpu_addr, dma_addr_t dma_handle) {
    (void)dev;
    (void)cpu_addr;
    int order = get_order(size);
    pmm_free(dma_handle, order);
}

// Bounce buffer tracking
struct bounce_buffer {
    void *orig_addr;
    phys_addr_t bounce_paddr;
    size_t size;
    enum dma_data_direction dir;
    struct bounce_buffer *next;
};

static spinlock_t bounce_lock = SPINLOCK_INIT;
static struct bounce_buffer *bounce_list = NULL;

dma_addr_t dma_map_single(device_t *dev, void *cpu_addr, size_t size, enum dma_data_direction dir) {
    phys_addr_t paddr = VIRT_TO_PHYS((virt_addr_t)cpu_addr);
    u64 mask = get_dma_mask(dev);

    if (paddr + size - 1 > mask) {
        // Needs bounce buffer
        int order = get_order(size);
        phys_addr_t bounce_paddr = pmm_alloc_32(order);
        if (!bounce_paddr)
            return 0; // Failure

        void *bounce_vaddr = (void *)PHYS_TO_VIRT(bounce_paddr);
        if (dir == DMA_TO_DEVICE || dir == DMA_BIDIRECTIONAL) {
            memcpy(bounce_vaddr, cpu_addr, size);
        }

        struct bounce_buffer *bb = kmalloc(sizeof(*bb));
        if (!bb) {
            pmm_free(bounce_paddr, order);
            return 0;
        }

        bb->orig_addr = cpu_addr;
        bb->bounce_paddr = bounce_paddr;
        bb->size = size;
        bb->dir = dir;

        irqflags_t flags;
        flags = spinlock_lock_irqsave(&bounce_lock);
        bb->next = bounce_list;
        bounce_list = bb;
        spinlock_unlock_irqrestore(&bounce_lock, flags);

        return bounce_paddr;
    }

    return paddr;
}

void dma_unmap_single(device_t *dev, dma_addr_t dma_addr, size_t size, enum dma_data_direction dir) {
    (void)dev;
    irqflags_t flags;
    flags = spinlock_lock_irqsave(&bounce_lock);
    
    struct bounce_buffer **curr = &bounce_list;
    while (*curr) {
        struct bounce_buffer *bb = *curr;
        if (bb->bounce_paddr == dma_addr) {
            *curr = bb->next;
            spinlock_unlock_irqrestore(&bounce_lock, flags);

            void *bounce_vaddr = (void *)PHYS_TO_VIRT(bb->bounce_paddr);
            if (bb->dir == DMA_FROM_DEVICE || bb->dir == DMA_BIDIRECTIONAL) {
                memcpy(bb->orig_addr, bounce_vaddr, bb->size);
            }

            int order = get_order(bb->size);
            pmm_free(bb->bounce_paddr, order);
            kfree(bb);
            return;
        }
        curr = &(*curr)->next;
    }
    
    spinlock_unlock_irqrestore(&bounce_lock, flags);
}

int dma_map_sg(device_t *dev, struct scatterlist *sg, int nents, enum dma_data_direction dir) {
    struct scatterlist *s;
    int i;
    for_each_sg(sg, s, nents, i) {
        void *cpu_addr = (void *)PHYS_TO_VIRT(s->phys_addr + s->offset);
        s->dma_address = dma_map_single(dev, cpu_addr, s->length, dir);
        if (!s->dma_address) {
            // Unmap already mapped ones
            int j;
            struct scatterlist *s2;
            for_each_sg(sg, s2, i, j) {
                dma_unmap_single(dev, s2->dma_address, s2->length, dir);
            }
            return 0;
        }
        s->dma_length = s->length;
    }
    return nents;
}

void dma_unmap_sg(device_t *dev, struct scatterlist *sg, int nents, enum dma_data_direction dir) {
    struct scatterlist *s;
    int i;
    for_each_sg(sg, s, nents, i) {
        dma_unmap_single(dev, s->dma_address, s->dma_length, dir);
    }
}

void dma_sync_single_for_cpu(device_t *dev, dma_addr_t dma_handle, size_t size, enum dma_data_direction dir) {
    (void)dev;
    (void)size;
    (void)dir;
    // On x86_64 cache is coherent for DMA, so bounce buffers are the only concern here,
    // but full implementation of sync with bounce buffers requires finding the buffer.
    // For now, dma_unmap_single does the copy back. We do a quick check if it's a bounce buffer.
    irqflags_t flags;
    flags = spinlock_lock_irqsave(&bounce_lock);
    struct bounce_buffer *curr = bounce_list;
    while (curr) {
        if (curr->bounce_paddr == dma_handle) {
            if (dir == DMA_FROM_DEVICE || dir == DMA_BIDIRECTIONAL) {
                void *bounce_vaddr = (void *)PHYS_TO_VIRT(curr->bounce_paddr);
                memcpy(curr->orig_addr, bounce_vaddr, size);
            }
            break;
        }
        curr = curr->next;
    }
    spinlock_unlock_irqrestore(&bounce_lock, flags);
}

void dma_sync_single_for_device(device_t *dev, dma_addr_t dma_handle, size_t size, enum dma_data_direction dir) {
    (void)dev;
    (void)size;
    (void)dir;
    irqflags_t flags;
    flags = spinlock_lock_irqsave(&bounce_lock);
    struct bounce_buffer *curr = bounce_list;
    while (curr) {
        if (curr->bounce_paddr == dma_handle) {
            if (dir == DMA_TO_DEVICE || dir == DMA_BIDIRECTIONAL) {
                void *bounce_vaddr = (void *)PHYS_TO_VIRT(curr->bounce_paddr);
                memcpy(bounce_vaddr, curr->orig_addr, size);
            }
            break;
        }
        curr = curr->next;
    }
    spinlock_unlock_irqrestore(&bounce_lock, flags);
}

// DMA Pool implementation
struct dma_pool_page {
    phys_addr_t paddr;
    void *vaddr;
    u8 *bitmap;
    struct dma_pool_page *next;
};

struct dma_pool {
    char name[32];
    device_t *dev;
    size_t size;
    size_t align;
    size_t boundary;
    struct dma_pool_page *pages;
    spinlock_t lock;
    int objs_per_page;
};

struct dma_pool *dma_pool_create(const char *name, device_t *dev, size_t size, size_t align, size_t boundary) {
    struct dma_pool *pool = kmalloc(sizeof(*pool));
    if (!pool) return NULL;
    
    strncpy(pool->name, name, sizeof(pool->name) - 1);
    pool->name[sizeof(pool->name) - 1] = '\0';
    pool->dev = dev;
    
    // align size to 'align'
    if (align == 0) align = 1;
    pool->size = (size + align - 1) & ~(align - 1);
    pool->align = align;
    pool->boundary = boundary; // boundary not fully supported in this simple impl
    pool->pages = NULL;
    spinlock_init(&pool->lock);
    pool->objs_per_page = PAGE_SIZE / pool->size;
    
    return pool;
}

void dma_pool_destroy(struct dma_pool *pool) {
    if (!pool) return;
    struct dma_pool_page *curr = pool->pages;
    while (curr) {
        struct dma_pool_page *next = curr->next;
        dma_free_coherent(pool->dev, PAGE_SIZE, curr->vaddr, curr->paddr);
        kfree(curr->bitmap);
        kfree(curr);
        curr = next;
    }
    kfree(pool);
}

void *dma_pool_alloc(struct dma_pool *pool, dma_addr_t *dma_handle) {
    irqflags_t flags;
    flags = spinlock_lock_irqsave(&pool->lock);
    
    struct dma_pool_page *curr = pool->pages;
    while (curr) {
        for (int i = 0; i < pool->objs_per_page; i++) {
            if (!(curr->bitmap[i / 8] & (1 << (i % 8)))) {
                curr->bitmap[i / 8] |= (1 << (i % 8));
                spinlock_unlock_irqrestore(&pool->lock, flags);
                
                *dma_handle = curr->paddr + (i * pool->size);
                return (u8 *)curr->vaddr + (i * pool->size);
            }
        }
        curr = curr->next;
    }
    
    // Allocate new page
    struct dma_pool_page *new_page = kmalloc(sizeof(*new_page));
    if (!new_page) {
        spinlock_unlock_irqrestore(&pool->lock, flags);
        return NULL;
    }
    
    new_page->vaddr = dma_alloc_coherent(pool->dev, PAGE_SIZE, &new_page->paddr);
    if (!new_page->vaddr) {
        kfree(new_page);
        spinlock_unlock_irqrestore(&pool->lock, flags);
        return NULL;
    }
    
    int bitmap_size = (pool->objs_per_page + 7) / 8;
    new_page->bitmap = kmalloc(bitmap_size);
    if (!new_page->bitmap) {
        dma_free_coherent(pool->dev, PAGE_SIZE, new_page->vaddr, new_page->paddr);
        kfree(new_page);
        spinlock_unlock_irqrestore(&pool->lock, flags);
        return NULL;
    }
    memset(new_page->bitmap, 0, bitmap_size);
    
    // allocate first object
    new_page->bitmap[0] |= 1;
    *dma_handle = new_page->paddr;
    void *ret = new_page->vaddr;
    
    new_page->next = pool->pages;
    pool->pages = new_page;
    
    spinlock_unlock_irqrestore(&pool->lock, flags);
    return ret;
}

void dma_pool_free(struct dma_pool *pool, void *vaddr, dma_addr_t dma_handle) {
    irqflags_t flags;
    flags = spinlock_lock_irqsave(&pool->lock);
    
    struct dma_pool_page *curr = pool->pages;
    while (curr) {
        if (dma_handle >= curr->paddr && dma_handle < curr->paddr + PAGE_SIZE) {
            int i = (dma_handle - curr->paddr) / pool->size;
            curr->bitmap[i / 8] &= ~(1 << (i % 8));
            break;
        }
        curr = curr->next;
    }
    
    spinlock_unlock_irqrestore(&pool->lock, flags);
}

void sg_init_table(struct scatterlist *sg, unsigned int nents) {
    memset(sg, 0, sizeof(*sg) * nents);
}

void sg_set_buf(struct scatterlist *sg, const void *buf, unsigned int buflen) {
    sg->phys_addr = VIRT_TO_PHYS((virt_addr_t)buf);
    sg->offset = 0;
    sg->length = buflen;
}

void sg_init_one(struct scatterlist *sg, const void *buf, unsigned int buflen) {
    sg_init_table(sg, 1);
    sg_set_buf(sg, buf, buflen);
}
