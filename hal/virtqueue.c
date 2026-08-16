/* ============================================================================
 * AzamiOS — VirtIO Virtqueue Implementation
 * File: hal/virtqueue.c
 * ============================================================================ */

#include "virtqueue.h"
#include "../kernel/mm/kmalloc.h"
#include "../arch/x86_64/mm/vmm.h"
#include "../kernel/mm/pmm.h"
#include <azami/debug.h>
#include "../include/azami/defs.h"

unsigned int vring_size(unsigned int queue_size, unsigned long align)
{
    unsigned int size;

    /* Descriptors and Available ring */
    size = queue_size * sizeof(struct vring_desc);
    size += sizeof(u16) * (3 + queue_size);
    
    /* Align up to page boundary for used ring */
    size = ALIGN_UP(size, align);

    /* Used ring */
    size += sizeof(u16) * 3 + sizeof(struct vring_used_elem) * queue_size;

    return size;
}

virtqueue_t *virtqueue_create(u16 queue_size)
{
    virtqueue_t *vq = (virtqueue_t *)kzalloc(sizeof(virtqueue_t));
    if (!vq) return NULL;

    vq->queue_size = queue_size;
    vq->last_used_idx = 0;
    vq->free_head = 0;
    vq->num_free = queue_size;

    vq->free_list = (u16 *)kzalloc(sizeof(u16) * queue_size);
    vq->data = (void **)kzalloc(sizeof(void *) * queue_size);
    if (!vq->free_list || !vq->data) {
        if (vq->free_list) kfree(vq->free_list);
        if (vq->data) kfree(vq->data);
        kfree(vq);
        return NULL;
    }

    /* Initialize free list */
    for (u16 i = 0; i < queue_size - 1; i++) {
        vq->free_list[i] = i + 1;
    }
    vq->free_list[queue_size - 1] = 0xFFFF; /* End of list */

    /* Allocate physical memory for the queue (needs to be contiguous) */
    unsigned int ring_sz = vring_size(queue_size, 4096);
    unsigned int pages = (unsigned int)ALIGN_UP(ring_sz, 4096) / 4096;
    
    vq->queue_phys = pmm_alloc_pages(pages);
    if (!vq->queue_phys) {
        kfree(vq->free_list);
        kfree(vq->data);
        kfree(vq);
        return NULL;
    }

    vq->queue_mem = (void *)PHYS_TO_VIRT(vq->queue_phys);
    __builtin_memset(vq->queue_mem, 0, pages * 4096);

    /* Initialize pointers into the queue memory */
    u8 *ptr = (u8 *)vq->queue_mem;
    vq->desc = (struct vring_desc *)ptr;
    ptr += queue_size * sizeof(struct vring_desc);
    
    vq->avail = (struct vring_avail *)ptr;
    ptr += sizeof(u16) * (3 + queue_size);
    
    ptr = (u8 *)ALIGN_UP((uintptr_t)ptr, 4096);
    vq->used = (struct vring_used *)ptr;

    /* Initialize descriptor next pointers */
    for (u16 i = 0; i < queue_size - 1; i++) {
        vq->desc[i].next = i + 1;
    }

    return vq;
}

int virtqueue_add_chain(virtqueue_t *vq, phys_addr_t *phys_addrs, u32 *lens, bool *is_write, u32 num, void *cookie)
{
    if (vq->num_free < num || num == 0) {
        return -1;
    }

    u16 head = vq->free_head;
    u16 current = head;

    for (u32 i = 0; i < num; i++) {
        current = vq->free_head;
        vq->free_head = vq->free_list[current];
        vq->num_free--;

        vq->desc[current].addr = phys_addrs[i];
        vq->desc[current].len = lens[i];
        vq->desc[current].flags = 0;
        
        if (is_write[i]) {
            vq->desc[current].flags |= VRING_DESC_F_WRITE;
        }

        if (i < num - 1) {
            vq->desc[current].flags |= VRING_DESC_F_NEXT;
            vq->desc[current].next = vq->free_head;
        }
    }

    vq->data[head] = cookie;

    /* Add the head to the available ring */
    u16 avail_idx = vq->avail->idx & (vq->queue_size - 1);
    vq->avail->ring[avail_idx] = head;
    
    __asm__ volatile("sfence" ::: "memory");
    
    vq->avail->idx++;

    return 0;
}

int virtqueue_add_buf(virtqueue_t *vq, phys_addr_t phys_addr, u32 len, bool is_write, void *cookie)
{
    return virtqueue_add_chain(vq, &phys_addr, &len, &is_write, 1, cookie);
}

void virtqueue_kick(virtqueue_t *vq)
{
    (void)vq;
    __asm__ volatile("sfence" ::: "memory");
}

void *virtqueue_get_used(virtqueue_t *vq, u32 *len)
{
    if (vq->last_used_idx == vq->used->idx) {
        return NULL; /* Nothing used yet */
    }

    __asm__ volatile("lfence" ::: "memory");

    u16 used_ring_idx = vq->last_used_idx & (vq->queue_size - 1);
    u32 id = vq->used->ring[used_ring_idx].id;
    if (len) {
        *len = vq->used->ring[used_ring_idx].len;
    }

    void *cookie = vq->data[id];
    vq->data[id] = NULL;

    /* Free the chain of descriptors */
    u16 head = (u16)id;
    u16 current = head;
    u16 count = 1;
    
    while (vq->desc[current].flags & VRING_DESC_F_NEXT) {
        u16 next = vq->desc[current].next;
        vq->free_list[current] = next;
        current = next;
        count++;
    }

    /* Link it back into the free list */
    vq->free_list[current] = vq->free_head;
    vq->free_head = head;
    vq->num_free += count;

    vq->last_used_idx++;
    return cookie;
}
