/* ============================================================================
 * AzamiOS — USB Mass Storage Class Driver (Bulk-Only Transport + SCSI)
 * File: drivers/usb/class/usbstorage.h
 *
 * Implements the USB Mass Storage Class Bulk-Only Transport (BOT) Rev 1.0
 * with SCSI transparent command set (SPC/SBC).
 * Binds to any USB flash drive, external SSD/HDD, or USB card reader
 * (Interface Class 0x08, SubClass 0x06, Protocol 0x50).
 * ============================================================================ */
#pragma once

#include "../core/usb.h"
#include "../../block/block.h"
#include "../../../arch/x86_64/cpu/spinlock.h"

/* USB Mass Storage Subclass & Protocol definitions */
#define USB_SUBCLASS_RBC           0x01   /* Reduced Block Commands (e.g. Flash) */
#define USB_SUBCLASS_ATAPI         0x02   /* CD/DVD ATAPI                        */
#define USB_SUBCLASS_QIC157        0x03   /* QIC-157 Tape                        */
#define USB_SUBCLASS_UFI           0x04   /* Floppy (UFI)                        */
#define USB_SUBCLASS_SFF8070I      0x05   /* SFF-8070i                           */
#define USB_SUBCLASS_SCSI          0x06   /* SCSI transparent command set        */

#define USB_PROTO_CBI_INT          0x00   /* Control/Bulk/Interrupt with IRQ     */
#define USB_PROTO_CBI_NO_INT       0x01   /* Control/Bulk/Interrupt no IRQ       */
#define USB_PROTO_BULK_ONLY        0x50   /* Bulk-Only Transport (BOT)           */

/* BOT Class-Specific Requests */
#define USB_BOT_REQ_RESET          0xFF   /* Bulk-Only Mass Storage Reset        */
#define USB_BOT_REQ_GET_MAX_LUN    0xFE   /* Get Max LUN                         */

/* Command Block Wrapper (CBW) — 31 bytes, little-endian */
#define USB_CBW_SIGNATURE          0x43425355U /* "USBC" */
#define USB_CBW_FLAGS_DIR_IN       0x80
#define USB_CBW_FLAGS_DIR_OUT      0x00

typedef struct __attribute__((packed)) {
    u32 dCBWSignature;          /* 0x43425355 */
    u32 dCBWTag;                /* Unique tag matching CSW */
    u32 dCBWDataTransferLength; /* Number of data bytes expected */
    u8  bmCBWFlags;             /* Direction: 0x80 = IN, 0x00 = OUT */
    u8  bCBWLUN;                /* Target LUN (0-15) */
    u8  bCBWCBLength;           /* Command block length (1-16) */
    u8  CBWCB[16];              /* SCSI Command Descriptor Block */
} usb_cbw_t;

/* Command Status Wrapper (CSW) — 13 bytes, little-endian */
#define USB_CSW_SIGNATURE          0x53425355U /* "USBS" */
#define USB_CSW_STATUS_PASSED      0x00
#define USB_CSW_STATUS_FAILED      0x01
#define USB_CSW_STATUS_PHASE_ERR   0x02

typedef struct __attribute__((packed)) {
    u32 dCSWSignature;          /* 0x53425355 */
    u32 dCSWTag;                /* Matches CBW tag */
    u32 dCSWDataResidue;        /* Difference between requested and actual transfer */
    u8  bCSWStatus;             /* Command status (0 = success) */
} usb_csw_t;

/* SCSI Command Opcodes */
#define SCSI_TEST_UNIT_READY       0x00
#define SCSI_REQUEST_SENSE         0x03
#define SCSI_INQUIRY               0x12
#define SCSI_MODE_SENSE_6          0x1A
#define SCSI_START_STOP_UNIT       0x1B
#define SCSI_READ_CAPACITY_10      0x25
#define SCSI_READ_10               0x28
#define SCSI_WRITE_10              0x2A
#define SCSI_SYNCHRONIZE_CACHE_10  0x35

/* Per-device state for a USB Mass Storage drive */
typedef struct usb_storage_dev {
    usb_device_t              *udev;
    u8                         iface_num;
    u8                         max_lun;
    u32                        tag;
    const usb_endpoint_descriptor_t *ep_in;
    const usb_endpoint_descriptor_t *ep_out;

    block_dev_t                bdev;
    block_ops_t                bops;

    char                       vendor[9];
    char                       product[17];
    char                       revision[5];

    spinlock_t                 lock;
    bool                       ready;
} usb_storage_dev_t;

/* Driver export */
extern const usb_class_driver_t usb_storage_driver;
