/* ============================================================================
 * AzamiOS — VirtIO Virtqueue Implementation
 * File: hal/virtqueue.h
 *
 * Implements the VirtIO split virtqueue structure and operations.
 * ============================================================================ */
#pragma once

#include "../include/azami/types.h"

/* Virtqueue descriptor flags */
#define VRING_DESC_F_NEXT       1
#define VRING_DESC_F_WRITE      2
#define VRING_DESC_F_INDIRECT   4

/* Virtqueue ring flags */
#define VRING_AVAIL_F_NO_INTERRUPT 1
#define VRING_USED_F_NO_NOTIFY     1

/* Virtqueue Descriptor */
struct vring_desc {
    u64 addr;
    u32 len;
    u16 flags;
    u16 next;
} __attribute__((packed));

/* Available Ring */
struct vring_avail {
    u16 flags;
    u16 idx;
    u16 ring[];
    /* u16 used_event is immediately after the ring */
} __attribute__((packed));

/* Used Ring Entry */
struct vring_used_elem {
    u32 id;
    u32 len;
} __attribute__((packed));

/* Used Ring */
struct vring_used {
    u16 flags;
    u16 idx;
    struct vring_used_elem ring[];
    /* u16 avail_event is immediately after the ring */
} __attribute__((packed));

/* Virtqueue structure used by the driver */
typedef struct virtqueue {
    u16 queue_size;
    u16 last_used_idx;
    u16 free_head;
    u16 num_free;
    
    struct vring_desc  *desc;
    struct vring_avail *avail;
    struct vring_used  *used;
    
    void *queue_mem;    /* Physical memory allocated for the queue */
    phys_addr_t queue_phys; /* Physical address of the memory */
    
    u16 *free_list;     /* Keeps track of next free descriptor in software */
    void **data;        /* Cookie per descriptor (for returning to caller) */
} virtqueue_t;

/**
 * vring_size - Compute total size of the virtqueue memory.
 * @queue_size: must be power of 2.
 * @align: alignment required (usually 4096).
 */
unsigned int vring_size(unsigned int queue_size, unsigned long align);

/**
 * virtqueue_create - Allocate and initialize a virtqueue.
 * @queue_size: The number of descriptors (must be power of 2).
 */
virtqueue_t *virtqueue_create(u16 queue_size);

/**
 * virtqueue_add_buf - Add a single buffer to the virtqueue.
 * @vq: Virtqueue.
 * @phys_addr: Physical address of the buffer.
 * @len: Length of the buffer.
 * @is_write: True if the device will write to the buffer.
 * @cookie: An opaque pointer returned when the buffer is used.
 * Returns 0 on success, negative on error.
 */
int virtqueue_add_buf(virtqueue_t *vq, phys_addr_t phys_addr, u32 len, bool is_write, void *cookie);

/**
 * virtqueue_add_chain - Add a chain of buffers (e.g. out + in).
 * @vq: Virtqueue.
 * @phys_addrs: Array of physical addresses.
 * @lens: Array of lengths.
 * @is_write: Array of booleans (true if device writes).
 * @num: Number of buffers in the chain.
 * @cookie: Opaque pointer attached to the head descriptor.
 */
int virtqueue_add_chain(virtqueue_t *vq, phys_addr_t *phys_addrs, u32 *lens, bool *is_write, u32 num, void *cookie);

/**
 * virtqueue_kick - Make the available descriptors visible to the device.
 * (The caller is responsible for notifying the device via PCI register after).
 */
void virtqueue_kick(virtqueue_t *vq);

/**
 * virtqueue_get_used - Check for and return a used buffer.
 * @vq: Virtqueue.
 * @len: Optional out parameter for the length written by the device.
 * Returns the cookie associated with the buffer, or NULL if none used.
 */
void *virtqueue_get_used(virtqueue_t *vq, u32 *len);
