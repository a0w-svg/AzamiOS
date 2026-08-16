/* ============================================================================
 * AzamiOS — VirtIO PCI Transport Implementation
 * File: hal/virtio_pci.h
 *
 * Implements the VirtIO 1.0 (modern) PCI transport layer.
 * ============================================================================ */
#pragma once

#include "../include/azami/types.h"
#include "pci.h"
#include "virtqueue.h"

/* VirtIO PCI capability types */
#define VIRTIO_PCI_CAP_COMMON_CFG 1
#define VIRTIO_PCI_CAP_NOTIFY_CFG 2
#define VIRTIO_PCI_CAP_ISR_CFG    3
#define VIRTIO_PCI_CAP_DEVICE_CFG 4
#define VIRTIO_PCI_CAP_PCI_CFG    5

/* VirtIO device status bits */
#define VIRTIO_CONFIG_S_ACKNOWLEDGE     1
#define VIRTIO_CONFIG_S_DRIVER          2
#define VIRTIO_CONFIG_S_DRIVER_OK       4
#define VIRTIO_CONFIG_S_FEATURES_OK     8
#define VIRTIO_CONFIG_S_NEEDS_RESET     64
#define VIRTIO_CONFIG_S_FAILED          128

/* Standard VirtIO Features */
#define VIRTIO_F_VERSION_1              32

/* PCI Capability Header (Standard PCI) */
struct pci_cap_hdr {
    u8 cap_vndr;    /* Generic PCI field: PCI_CAP_ID_VNDR = 0x09 */
    u8 cap_next;    /* Next ptr */
    u8 cap_len;     /* Capability length */
    u8 cfg_type;    /* VIRTIO_PCI_CAP_* */
    u8 bar;         /* Where to find it */
    u8 padding[3];
    u32 offset;     /* Offset within bar */
    u32 length;     /* Length of the structure */
} __attribute__((packed));

/* Common Configuration (VIRTIO_PCI_CAP_COMMON_CFG) */
struct virtio_pci_common_cfg {
    u32 device_feature_select;  /* read-write */
    u32 device_feature;         /* read-only */
    u32 driver_feature_select;  /* read-write */
    u32 driver_feature;         /* read-write */
    u16 msix_config;            /* read-write */
    u16 num_queues;             /* read-only */
    u8 device_status;           /* read-write */
    u8 config_generation;       /* read-only */
    u16 queue_select;           /* read-write */
    u16 queue_size;             /* read-write, power of 2 */
    u16 queue_msix_vector;      /* read-write */
    u16 queue_enable;           /* read-write */
    u16 queue_notify_off;       /* read-only */
    u64 queue_desc;             /* read-write */
    u64 queue_avail;            /* read-write */
    u64 queue_used;             /* read-write */
} __attribute__((packed));

/* Notify Configuration (VIRTIO_PCI_CAP_NOTIFY_CFG) */
struct virtio_pci_notify_cap {
    struct pci_cap_hdr cap;
    u32 notify_off_multiplier; /* Multiplier for queue_notify_off */
} __attribute__((packed));

/* VirtIO Device Structure */
typedef struct virtio_pci_device {
    device_t *pci_dev;
    
    /* Mapped Configuration Spaces */
    volatile struct virtio_pci_common_cfg *common_cfg;
    volatile u8 *isr_cfg;
    volatile u8 *device_cfg;
    volatile u8 *notify_base;
    u32 notify_multiplier;
    
} virtio_pci_device_t;

/**
 * virtio_pci_init_device - Parse VirtIO capabilities and map BARs.
 * @dev: The PCI device.
 * @virtio_dev: The output virtio device structure.
 * Returns 0 on success.
 */
int virtio_pci_init_device(device_t *dev, virtio_pci_device_t *virtio_dev);

/**
 * virtio_pci_setup_queue - Configure a virtqueue with the device.
 * @virtio_dev: The virtio device.
 * @queue_index: The index of the queue to setup.
 * Returns a pointer to the created virtqueue or NULL.
 */
virtqueue_t *virtio_pci_setup_queue(virtio_pci_device_t *virtio_dev, u16 queue_index);

/**
 * virtio_pci_notify - Notify the device that a queue has been updated.
 * @virtio_dev: The virtio device.
 * @queue_index: The index of the queue.
 * @vq: The virtqueue object.
 */
void virtio_pci_notify(virtio_pci_device_t *virtio_dev, u16 queue_index, virtqueue_t *vq);

/**
 * virtio_pci_set_status - Set the device status byte.
 */
void virtio_pci_set_status(virtio_pci_device_t *virtio_dev, u8 status);

/**
 * virtio_pci_get_status - Get the device status byte.
 */
u8 virtio_pci_get_status(virtio_pci_device_t *virtio_dev);

/**
 * virtio_pci_negotiate_features - Negotiate features with the device.
 */
bool virtio_pci_negotiate_features(virtio_pci_device_t *virtio_dev, u64 requested_features);
