/* ============================================================================
 * AzamiOS — VirtIO Block Device Driver Header
 * File: drivers/block/virtio_blk.h
 * ============================================================================ */
#pragma once

#include "../../include/azami/types.h"
#include "../../hal/device.h"
#include "../../hal/virtio_pci.h"
#include "../../hal/virtqueue.h"
#include "../../arch/x86_64/cpu/spinlock.h"
#include "block.h"

#define VIRTIO_BLK_T_IN           0
#define VIRTIO_BLK_T_OUT          1
#define VIRTIO_BLK_T_FLUSH        4
#define VIRTIO_BLK_T_GET_ID       8

#define VIRTIO_BLK_S_OK           0
#define VIRTIO_BLK_S_IOERR        1
#define VIRTIO_BLK_S_UNSUPP       2

typedef struct __attribute__((packed)) {
    u32 type;
    u32 reserved;
    u64 sector;
} virtio_blk_req_hdr_t;

/* Up to this many virtio-blk-pci devices can bind — a small fixed pool
 * rather than a dynamic allocation, since the driver model never expects
 * more than a handful of disks. */
#define VIRTIO_BLK_MAX_DEVICES 4

typedef struct {
    virtio_pci_device_t vpci;
    virtqueue_t         *vq;
    u64                  capacity_sectors;
    block_dev_t          bdev;
    spinlock_t           lock;   /* one per device, not shared across disks */
    bool                 active;
} virtio_blk_dev_t;

/** virtio_blk_init() — Register the PCI driver; probe() binds to every
 *  matching virtio-blk-pci device found (up to VIRTIO_BLK_MAX_DEVICES), so
 *  this is safe to call whether or not the host has any. */
void virtio_blk_init(void);
