/* ============================================================================
 * AzamiOS — VirtIO SCSI Host Driver Header
 * File: drivers/block/virtio_scsi.h
 *
 * A minimal virtio-scsi-pci initiator: one request queue, one command in
 * flight at a time (same design as virtio_blk.c), READ(10)/WRITE(10) only.
 * Enough to drive QEMU's `-device virtio-scsi-pci` + `-device scsi-hd`, which
 * is by far the common case; multi-queue, CDB sizes other than 32 bytes, and
 * anything beyond direct-access disks (opcode-wise) are out of scope here.
 * ============================================================================ */
#pragma once

#include "../../include/azami/types.h"
#include "../../hal/device.h"
#include "../../hal/virtio_pci.h"
#include "../../hal/virtqueue.h"
#include "../../arch/x86_64/cpu/spinlock.h"
#include "block.h"

/* Device configuration space (virtio-scsi §5.6.4). Read once at probe time
 * purely to bound the target scan below — this driver does not negotiate
 * multiqueue or a non-default CDB/sense size. */
struct virtio_scsi_config {
    u32 num_queues;
    u32 seg_max;
    u32 max_sectors;
    u32 cmd_per_lun;
    u32 event_info_size;
    u32 sense_size;
    u32 cdb_size;
    u16 max_channel;
    u16 max_target;
    u32 max_lun;
} __attribute__((packed));

/* This driver's fixed choice for both, matching QEMU virtio-scsi-pci's
 * defaults. A device that reports a larger cdb_size/sense_size in its config
 * space still works fine against these — the request/response layout below
 * is exactly what the spec calls "the driver may choose the CDB/sense size",
 * and a real SCSI target never needs more than a 10-byte CDB or a fraction
 * of 96 bytes of sense data for the commands this driver issues. */
#define VIRTIO_SCSI_CDB_SIZE   32
#define VIRTIO_SCSI_SENSE_SIZE 96

struct virtio_scsi_cmd_req {
    u8  lun[8];
    u64 id;
    u8  task_attr;
    u8  prio;
    u8  crn;
    u8  cdb[VIRTIO_SCSI_CDB_SIZE];
} __attribute__((packed));

struct virtio_scsi_cmd_resp {
    u32 sense_len;
    u32 residual;
    u16 status_qualifier;
    u8  status;
    u8  response;
    u8  sense[VIRTIO_SCSI_SENSE_SIZE];
} __attribute__((packed));

/* response field (transport-level outcome, distinct from .status which is
 * the SCSI status byte the target itself returned). */
#define VIRTIO_SCSI_S_OK 0

#define VIRTIO_SCSI_MAX_TARGETS 8   /* targets 0..7, LUN 0 only on each */

typedef struct virtio_scsi_disk {
    struct virtio_scsi_dev *host;   /* back-pointer to the HBA that owns it */
    u8          target;
    u64         capacity_sectors;
    u32         sector_size;
    block_dev_t bdev;
} virtio_scsi_disk_t;

typedef struct virtio_scsi_dev {
    virtio_pci_device_t vpci;
    virtqueue_t         *control_vq;   /* queue 0 */
    virtqueue_t         *event_vq;     /* queue 1 */
    virtqueue_t         *request_vq;   /* queue 2: the only one this driver uses */
    spinlock_t          lock;          /* serializes request_vq submitters   */
    bool                active;
    u32                 ndisks;
    virtio_scsi_disk_t  disks[VIRTIO_SCSI_MAX_TARGETS];
} virtio_scsi_dev_t;

/** virtio_scsi_init() — Register the PCI driver; probe() binds to a
 *  matching virtio-scsi-pci device and scans it for disks automatically, so
 *  this is safe to call whether or not the host has one. */
void virtio_scsi_init(void);
