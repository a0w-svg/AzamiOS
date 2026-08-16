/* ============================================================================
 * AzamiOS — VirtIO Block Device Driver Header
 * File: drivers/block/virtio_blk.h
 * ============================================================================ */
#pragma once

#include "../../include/azami/types.h"
#include "../../hal/device.h"
#include "../../hal/virtio_pci.h"
#include "../../hal/virtqueue.h"
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

typedef struct {
    virtio_pci_device_t vpci;
    virtqueue_t         *vq;
    u64                  capacity_sectors;
    block_dev_t          bdev;
} virtio_blk_dev_t;

int virtio_blk_init(device_t *pci_dev);
void virtio_blk_probe_all(void);
