/* ============================================================================
 * AzamiOS — VirtIO 9P / VirtFS Host-Guest Shared Folder Driver Header
 * File: drivers/misc/virtio_9p.h
 * ============================================================================ */
#pragma once

#include "../../include/azami/defs.h"
#include "../../hal/virtio_pci.h"
#include "../../hal/virtqueue.h"
#include "../../drivers/base/pci_bus.h"

#define VIRTIO_ID_9P              9

#define VIRTIO_9P_TRANSITIONAL_ID 0x1009
#define VIRTIO_9P_MODERN_ID       0x1049

#define VIRTIO_9P_TAG_MAX         256

typedef struct virtio_9p_dev {
    virtio_pci_device_t vpci;
    virtqueue_t        *vq;
    char                tag[VIRTIO_9P_TAG_MAX];
    u16                 tag_len;
    bool                active;
} virtio_9p_dev_t;

/** virtio_9p_init() — Register VirtIO 9P driver on the PCI bus. */
int virtio_9p_init(void);

/** virtio_9p_get_tag(buf, maxlen) — Retrieve the host filesystem mount tag. */
int virtio_9p_get_tag(char *buf, size_t maxlen);
