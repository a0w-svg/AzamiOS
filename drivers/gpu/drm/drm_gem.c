/* ============================================================================
 * AzamiOS — DRM: GEM buffer objects
 * File: drivers/gpu/drm/drm_gem.c
 *
 * Buffer objects and the per-file handle namespace, following Linux's GEM:
 * an object is refcounted and lives on the card, while a *handle* is a small
 * integer private to one open file descriptor.  Two clients that share a
 * buffer through PRIME or flink each get their own handle for it.
 *
 * Placement is the driver's decision.  drm_gem_object_create() offers each
 * new object to driver->gem_place(); a driver that can scan out directly
 * fills in VRAM pages, and anything it declines is backed by ordinary system
 * pages and treated as a shadow buffer that page_flip()/dirty_fb() copies
 * into the scanout window.
 * ============================================================================ */

#define DEBUG 1
#include <azami/debug.h>
#include "drm_core.h"
#include "../../../kernel/mm/kmalloc.h"
#include "../../../kernel/mm/pmm.h"
#include "../../../kernel/lib/string.h"

/* mmap offsets are fake file offsets, not addresses; space them widely so a
 * client mapping one object can never walk into the next. */
#define DRM_MMAP_OFFSET_BASE   0x100000000ULL
#define DRM_MMAP_OFFSET_STEP   0x10000000ULL   /* 256 MiB per object */

/* ── Object lifetime ─────────────────────────────────────────────────────── */

drm_gem_object_t *drm_gem_object_create(drm_device_t *dev, u32 width, u32 height,
                                        u32 bpp, u32 pitch)
{
    if (!dev || width == 0 || height == 0) return NULL;

    size_t size   = (size_t)pitch * height;
    size_t npages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (npages == 0) return NULL;

    drm_gem_object_t *obj = (drm_gem_object_t *)kzalloc(sizeof(drm_gem_object_t));
    if (!obj) return NULL;

    obj->pages = (phys_addr_t *)kzalloc(npages * sizeof(phys_addr_t));
    if (!obj->pages) {
        kfree(obj);
        return NULL;
    }

    obj->dev      = dev;
    obj->width    = width;
    obj->height   = height;
    obj->bpp      = bpp;
    obj->pitch    = pitch;
    obj->size     = size;
    obj->npages   = npages;
    obj->refcount = 1;

    /* Give the driver first refusal on placement (VRAM, stolen memory, …). */
    int placed = -ENOSPC;
    if (dev->driver->gem_place) {
        placed = dev->driver->gem_place(dev, obj);
    }

    if (placed != 0) {
        obj->in_vram = false;
        for (size_t i = 0; i < npages; i++) {
            phys_addr_t page = pmm_alloc_page();
            if (!page) {
                for (size_t k = 0; k < i; k++) pmm_free_page(obj->pages[k]);
                kfree(obj->pages);
                kfree(obj);
                return NULL;
            }
            memset(PHYS_TO_VIRT(page), 0, PAGE_SIZE);
            obj->pages[i] = page;
        }
    }

    spinlock_lock(&dev->lock);
    if (dev->next_mmap_offset == 0) dev->next_mmap_offset = DRM_MMAP_OFFSET_BASE;
    obj->mmap_offset = dev->next_mmap_offset;
    dev->next_mmap_offset += ALIGN_UP(size, DRM_MMAP_OFFSET_STEP);
    obj->next     = dev->gem_list;
    dev->gem_list = obj;
    spinlock_unlock(&dev->lock);

    return obj;
}

void drm_gem_object_get(drm_gem_object_t *obj)
{
    if (obj) obj->refcount++;
}

void drm_gem_object_put(drm_device_t *dev, drm_gem_object_t *obj)
{
    if (!dev || !obj) return;
    if (--obj->refcount > 0) return;

    drm_gem_object_t **pp = &dev->gem_list;
    while (*pp) {
        if (*pp == obj) { *pp = obj->next; break; }
        pp = &(*pp)->next;
    }

    if (dev->driver->gem_release) {
        dev->driver->gem_release(dev, obj);
    }
    if (obj->pages) {
        /* VRAM pages belong to the adapter, not the page allocator. */
        if (!obj->in_vram) {
            for (size_t i = 0; i < obj->npages; i++) {
                if (obj->pages[i]) pmm_free_page(obj->pages[i]);
            }
        }
        kfree(obj->pages);
    }
    kfree(obj);
}

/* ── Per-file handles ────────────────────────────────────────────────────── */

u32 drm_gem_handle_create(drm_file_t *file, drm_gem_object_t *obj)
{
    if (!file || !obj) return 0;

    /* Handle 0 is reserved as "no object", matching Linux. */
    for (u32 i = 1; i < DRM_MAX_HANDLES; i++) {
        u32 h = file->next_handle + i;
        h = 1 + (h - 1) % (DRM_MAX_HANDLES - 1);
        if (file->handles[h]) continue;

        file->handles[h]  = obj;
        file->next_handle = h;
        drm_gem_object_get(obj);
        return h;
    }
    return 0;
}

drm_gem_object_t *drm_gem_handle_lookup(drm_file_t *file, u32 handle)
{
    if (!file || handle == 0 || handle >= DRM_MAX_HANDLES) return NULL;
    return file->handles[handle];
}

int drm_gem_handle_delete(drm_file_t *file, u32 handle)
{
    drm_gem_object_t *obj = drm_gem_handle_lookup(file, handle);
    if (!obj) return -EINVAL;

    file->handles[handle] = NULL;
    drm_gem_object_put(file->dev, obj);
    return 0;
}

void drm_gem_release_all(drm_file_t *file)
{
    if (!file) return;
    for (u32 h = 1; h < DRM_MAX_HANDLES; h++) {
        if (!file->handles[h]) continue;
        drm_gem_object_t *obj = file->handles[h];
        file->handles[h] = NULL;
        drm_gem_object_put(file->dev, obj);
    }
}

/* ── Lookups used by mmap and flink ──────────────────────────────────────── */

drm_gem_object_t *drm_gem_object_by_mmap_offset(drm_device_t *dev, u64 offset)
{
    if (!dev) return NULL;
    for (drm_gem_object_t *o = dev->gem_list; o; o = o->next) {
        if (o->mmap_offset == offset) return o;
        /* Tolerate a client mapping partway into an object. */
        if (offset > o->mmap_offset && offset < o->mmap_offset + o->size) return o;
    }
    return NULL;
}

drm_gem_object_t *drm_gem_object_by_name(drm_device_t *dev, u32 name)
{
    if (!dev || name == 0) return NULL;
    for (drm_gem_object_t *o = dev->gem_list; o; o = o->next) {
        if (o->name == name) return o;
    }
    return NULL;
}

u32 drm_gem_object_flink(drm_device_t *dev, drm_gem_object_t *obj)
{
    if (!dev || !obj) return 0;
    if (obj->name == 0) obj->name = ++dev->next_gem_name;
    return obj->name;
}

/* ── Kernel-side access ──────────────────────────────────────────────────── */

void *drm_gem_page_ptr(drm_gem_object_t *obj, size_t page_index)
{
    if (!obj || page_index >= obj->npages || !obj->pages[page_index]) return NULL;
    return PHYS_TO_VIRT(obj->pages[page_index]);
}

/*
 * Copy one rectangle of a buffer object into a scanout window.
 *
 * The destination is nearly always write-combining video memory, where writes
 * stream and reads crawl, so this only ever writes: it walks the source page
 * vector and pushes whole rows out.  Narrowing the rectangle is what makes a
 * shadow-buffered driver cheap — a compositor that changed one window copies
 * one window, not a screen.
 */
void drm_gem_blit_rect(drm_gem_object_t *src, void *dst_virt, u32 dst_pitch,
                       const drm_rect_t *clip, u32 bpp)
{
    if (!src || !dst_virt || !clip) return;

    size_t bytes_pp = (bpp + 7) / 8;
    if (bytes_pp == 0) return;

    u32 x1 = clip->x1, y1 = clip->y1;
    u32 x2 = clip->x2, y2 = clip->y2;

    if (y2 > src->height) y2 = src->height;
    if (x2 > src->pitch / bytes_pp)  x2 = src->pitch / bytes_pp;
    if (x2 > dst_pitch / bytes_pp)   x2 = dst_pitch / bytes_pp;
    if (x1 >= x2 || y1 >= y2) return;

    size_t row_bytes = (size_t)(x2 - x1) * bytes_pp;
    size_t x_off     = (size_t)x1 * bytes_pp;
    u8    *dst       = (u8 *)dst_virt;

    for (u32 y = y1; y < y2; y++) {
        size_t src_off = (size_t)y * src->pitch + x_off;
        size_t copied  = 0;

        /* The source is a page vector, so one row can straddle pages. */
        while (copied < row_bytes) {
            size_t page  = (src_off + copied) / PAGE_SIZE;
            size_t in_pg = (src_off + copied) % PAGE_SIZE;
            void  *sp    = drm_gem_page_ptr(src, page);
            if (!sp) return;

            size_t chunk = PAGE_SIZE - in_pg;
            if (chunk > row_bytes - copied) chunk = row_bytes - copied;

            memcpy(dst + (size_t)y * dst_pitch + x_off + copied,
                   (u8 *)sp + in_pg, chunk);
            copied += chunk;
        }
    }
}

void drm_gem_blit(drm_gem_object_t *src, void *dst_virt, u32 dst_pitch,
                  u32 width, u32 height, u32 bpp)
{
    drm_rect_t all = { 0, 0, width, height };
    drm_gem_blit_rect(src, dst_virt, dst_pitch, &all, bpp);
}
