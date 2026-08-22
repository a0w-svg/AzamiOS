/* ============================================================================
 * AzamiOS — VirtIO Hardware Random Number Generator Driver Header
 * File: drivers/misc/virtio_rng.h
 * ============================================================================ */
#pragma once

#include "../../include/azami/types.h"
#include "../../hal/device.h"
#include "../../hal/virtio_pci.h"
#include "../../hal/virtqueue.h"

#define VIRTIO_RNG_DEVICE_ID_LEGACY  0x1005
#define VIRTIO_RNG_DEVICE_ID_MODERN  0x1044

typedef struct virtio_rng_dev {
    virtio_pci_device_t vpci;
    virtqueue_t *vq;
    u32 irq;
    bool active;
} virtio_rng_dev_t;

/** virtio_rng_init() — Probe and initialize VirtIO-RNG device. */
int virtio_rng_init(device_t *dev);

/** virtio_rng_get_bytes() — Fill buffer with hardware random bytes. */
int virtio_rng_get_bytes(void *buf, size_t len);
