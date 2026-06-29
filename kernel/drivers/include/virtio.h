/**
 * kernel/drivers/include/virtio.h — AzamiOS VirtIO Paravirtualized PCI Driver
 */
#ifndef VIRTIO_H
#define VIRTIO_H

#include <stdint.h>
#include <stdbool.h>
#include "../../filesystem/include/vfs.h"

#define VIRTIO_VENDOR_ID 0x1AF4

/* Subsystem / Device IDs */
#define VIRTIO_DEV_NET     0x1000
#define VIRTIO_DEV_BLK     0x1001
#define VIRTIO_DEV_BALLOON 0x1002
#define VIRTIO_DEV_CONSOLE 0x1003
#define VIRTIO_DEV_RNG     0x1005

/* Modern PCI Device IDs (0x1040 + type) */
#define VIRTIO_DEV_MODERN_NET     0x1041
#define VIRTIO_DEV_MODERN_BLK     0x1042
#define VIRTIO_DEV_MODERN_CONSOLE 0x1043
#define VIRTIO_DEV_MODERN_RNG     0x1044

/* VirtIO Status bits */
#define VIRTIO_STATUS_ACKNOWLEDGE 1
#define VIRTIO_STATUS_DRIVER      2
#define VIRTIO_STATUS_DRIVER_OK   4
#define VIRTIO_STATUS_FEATURES_OK 8
#define VIRTIO_STATUS_FAILED      128

bool virtio_probe(void);
int  virtio_init(void);

#endif /* VIRTIO_H */
