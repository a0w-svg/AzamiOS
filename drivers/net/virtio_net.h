/* ============================================================================
 * AzamiOS — VirtIO Network Device Driver Header
 * File: drivers/net/virtio_net.h
 * ============================================================================ */
#pragma once

#include "../../include/azami/types.h"
#include "../../hal/device.h"
#include "../../hal/virtio_pci.h"
#include "../../hal/virtqueue.h"

#define VIRTIO_NET_F_MAC        (1ULL << 5)
#define VIRTIO_NET_F_STATUS     (1ULL << 16)

struct virtio_net_hdr {
    u8  flags;
    u8  gso_type;
    u16 hdr_len;
    u16 gso_size;
    u16 csum_start;
    u16 csum_offset;
    u16 num_buffers;
} __attribute__((packed));

typedef struct {
    virtio_pci_device_t vpci;
    virtqueue_t         *rx_vq;
    virtqueue_t         *tx_vq;
    u8                  mac[6];
    bool                active;
} virtio_net_dev_t;

int virtio_net_init(device_t *pci_dev);
s64 virtio_net_send_packet(const void *data, size_t len);
void virtio_net_get_mac(u8 mac_out[6]);
void virtio_net_poll(void);
