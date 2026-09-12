/* ============================================================================
 * AzamiOS — VMware VMXNET3 Paravirtualized Network Adapter Header
 * File: drivers/net/vmxnet3.h
 * ============================================================================ */
#pragma once

#include "../../include/azami/defs.h"
#include "../../hal/pci.h"
#include "../../drivers/base/pci_bus.h"

/* ── PCI Identifiers ─────────────────────────────────────────────────────── */
#define PCI_VENDOR_VMWARE          0x15AD
#define PCI_DEVICE_VMWARE_VMXNET3  0x07B0

/* ── MMIO Register Offsets (BAR0) ────────────────────────────────────────── */
#define VMXNET3_REG_VRRS           0x000  /* Version Report & Reset */
#define VMXNET3_REG_UVRS           0x008  /* UPT Version Select */
#define VMXNET3_REG_DSAL           0x010  /* Driver Shared Address Low */
#define VMXNET3_REG_DSAH           0x018  /* Driver Shared Address High */
#define VMXNET3_REG_CMD            0x020  /* Command Register */
#define VMXNET3_REG_MAC_L          0x028  /* MAC Address Low */
#define VMXNET3_REG_MAC_H          0x030  /* MAC Address High */
#define VMXNET3_REG_ICR            0x038  /* Interrupt Cause */
#define VMXNET3_REG_ECR            0x040  /* Event Cause */

/* ── Commands (VMXNET3_REG_CMD) ──────────────────────────────────────────── */
#define VMXNET3_CMD_ENABLE_DEV     0xCAFE0001
#define VMXNET3_CMD_DISABLE_DEV    0xCAFE0002
#define VMXNET3_CMD_RESET_DEV      0xCAFE0003
#define VMXNET3_CMD_QUIESCE_DEV    0xCAFE0004
#define VMXNET3_CMD_GET_STATUS     0xCAFE0005
#define VMXNET3_CMD_GET_NUM_QUEUES 0xCAFE0006
#define VMXNET3_CMD_GET_MAC_ADDR   0xCAFE0007

#define VMXNET3_MAGIC              0xBABE0001

/* ── Driver Shared Data Structure (Aligned to 8 bytes) ───────────────────── */
typedef struct __attribute__((packed, aligned(8))) vmxnet3_driver_shared {
    u32 magic;
    u32 pad;
    u32 version;
    u32 guest_os;
    u32 vmxnet3_revision;
    u32 upt_version;
    u64 upt_features;
    u64 driver_data;
    u64 queue_shared_pa;
    u32 queue_shared_len;
    u32 mtu;
    u16 num_tx_queues;
    u16 num_rx_queues;
    u32 rx_mode;
} vmxnet3_driver_shared_t;

/* ── Device State ────────────────────────────────────────────────────────── */
typedef struct vmxnet3_device {
    uintptr_t                mmio_base;
    size_t                   mmio_size;
    u8                       mac[6];
    u8                       irq;
    bool                     link_up;
    vmxnet3_driver_shared_t *shared;
    phys_addr_t              shared_phys;
    u32                      rx_packets;
    u32                      tx_packets;
} vmxnet3_device_t;

/** vmxnet3_init() — Register the VMXNET3 PCI driver in the unified driver model. */
int vmxnet3_init(void);

/** vmxnet3_get_mac(mac_out) — Copy the hardware MAC address of the adapter. */
void vmxnet3_get_mac(u8 mac_out[6]);
