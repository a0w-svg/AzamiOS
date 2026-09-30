/* ============================================================================
 * AzamiOS — USB Mass Storage Class Driver Implementation
 * File: drivers/usb/class/usbstorage.c
 *
 * Implements the USB Mass Storage Bulk-Only Transport (BOT) protocol and
 * transparent SCSI command execution (INQUIRY, READ CAPACITY, READ10, WRITE10,
 * TEST UNIT READY, SYNCHRONIZE CACHE).
 *
 * Registers matching devices directly with the AzamiOS block device layer
 * (/dev/sda, /dev/sdb, etc.), triggering automatic MBR/GPT partition scanning.
 * ============================================================================ */

#define DEBUG 1
#include "../../../include/azami/debug.h"
#include "usbstorage.h"
#include "../../block/block.h"
#include "../../../kernel/mm/kmalloc.h"
#include "../../../kernel/lib/string.h"
#include "../../../arch/x86_64/cpu/spinlock.h"

static u32 g_usb_disk_index = 0;

/* ── BOT Transport Helpers ────────────────────────────────────────────────── */

static int usb_storage_reset(usb_storage_dev_t *sdev)
{
    return usb_control_msg(sdev->udev,
                           USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
                           USB_BOT_REQ_RESET, 0, sdev->iface_num, NULL, 0);
}

static u8 usb_storage_get_max_lun(usb_storage_dev_t *sdev)
{
    u8 max_lun = 0;
    int rc = usb_control_msg(sdev->udev,
                             USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
                             USB_BOT_REQ_GET_MAX_LUN, 0, sdev->iface_num,
                             &max_lun, 1);
    if (rc < 0) {
        /* Many single-LUN drives STALL this request per BOT spec §3.2. Treat as LUN 0. */
        return 0;
    }
    return max_lun;
}

/**
 * Executes a full SCSI command over the USB Bulk-Only Transport:
 *   1. Send CBW (31 bytes) to Bulk-OUT
 *   2. Data phase (if data != NULL and data_len > 0)
 *   3. Receive CSW (13 bytes) from Bulk-IN
 */
static int usb_storage_scsi_command(usb_storage_dev_t *sdev,
                                    const u8 *cdb, u8 cdb_len,
                                    void *data, u32 data_len, bool is_in)
{
    if (!sdev || !sdev->udev || !sdev->udev->bus || !sdev->udev->bus->ops)
        return -ENODEV;
    if (!sdev->udev->bus->ops->bulk)
        return -ENOSYS;

    usb_cbw_t cbw;
    memset(&cbw, 0, sizeof(cbw));
    cbw.dCBWSignature          = USB_CBW_SIGNATURE;
    cbw.dCBWTag                = ++sdev->tag;
    cbw.dCBWDataTransferLength = data_len;
    cbw.bmCBWFlags             = is_in ? USB_CBW_FLAGS_DIR_IN : USB_CBW_FLAGS_DIR_OUT;
    cbw.bCBWLUN                = 0;
    cbw.bCBWCBLength           = cdb_len;
    memcpy(cbw.CBWCB, cdb, cdb_len > 16 ? 16 : cdb_len);

    /* Phase 1: Send CBW */
    int rc = sdev->udev->bus->ops->bulk(sdev->udev, sdev->ep_out, &cbw, sizeof(cbw), 0);
    if (rc < 0) {
        pr_debug("[USB-STORAGE] CBW send failed: %d\n", rc);
        usb_storage_reset(sdev);
        return rc;
    }

    /* Phase 2: Data transfer */
    if (data && data_len > 0) {
        const usb_endpoint_descriptor_t *ep = is_in ? sdev->ep_in : sdev->ep_out;
        rc = sdev->udev->bus->ops->bulk(sdev->udev, ep, data, data_len, 0);
        if (rc < 0) {
            pr_debug("[USB-STORAGE] Data phase (%s) failed: %d\n",
                     is_in ? "IN" : "OUT", rc);
        }
    }

    /* Phase 3: Receive CSW */
    usb_csw_t csw;
    memset(&csw, 0, sizeof(csw));
    rc = sdev->udev->bus->ops->bulk(sdev->udev, sdev->ep_in, &csw, sizeof(csw), 0);
    if (rc < 0) {
        pr_debug("[USB-STORAGE] CSW recv failed: %d\n", rc);
        usb_storage_reset(sdev);
        return rc;
    }

    if (csw.dCSWSignature != USB_CSW_SIGNATURE || csw.dCSWTag != cbw.dCBWTag) {
        pr_debug("[USB-STORAGE] Invalid CSW signature 0x%08x or tag mismatch\n",
                 csw.dCSWSignature);
        usb_storage_reset(sdev);
        return -EIO;
    }

    if (csw.bCSWStatus != USB_CSW_STATUS_PASSED) {
        return -EIO;
    }

    return 0;
}

/* ── SCSI Command Helpers ─────────────────────────────────────────────────── */

static int scsi_test_unit_ready(usb_storage_dev_t *sdev)
{
    u8 cdb[6] = { SCSI_TEST_UNIT_READY, 0, 0, 0, 0, 0 };
    return usb_storage_scsi_command(sdev, cdb, sizeof(cdb), NULL, 0, true);
}

static int scsi_request_sense(usb_storage_dev_t *sdev, u8 *sense_buf, u8 sense_len)
{
    u8 cdb[6] = { SCSI_REQUEST_SENSE, 0, 0, 0, sense_len, 0 };
    return usb_storage_scsi_command(sdev, cdb, sizeof(cdb), sense_buf, sense_len, true);
}

static int scsi_inquiry(usb_storage_dev_t *sdev)
{
    u8 inq_data[36];
    memset(inq_data, 0, sizeof(inq_data));
    u8 cdb[6] = { SCSI_INQUIRY, 0, 0, 0, sizeof(inq_data), 0 };

    int rc = usb_storage_scsi_command(sdev, cdb, sizeof(cdb), inq_data, sizeof(inq_data), true);
    if (rc < 0) return rc;

    /* Extract Vendor (bytes 8..15), Product (bytes 16..31), Rev (bytes 32..35) */
    memcpy(sdev->vendor, &inq_data[8], 8);
    sdev->vendor[8] = '\0';
    memcpy(sdev->product, &inq_data[16], 16);
    sdev->product[16] = '\0';
    memcpy(sdev->revision, &inq_data[32], 4);
    sdev->revision[4] = '\0';

    /* Trim trailing spaces */
    for (int i = 7; i >= 0 && sdev->vendor[i] == ' '; i--) sdev->vendor[i] = '\0';
    for (int i = 15; i >= 0 && sdev->product[i] == ' '; i--) sdev->product[i] = '\0';
    for (int i = 3; i >= 0 && sdev->revision[i] == ' '; i--) sdev->revision[i] = '\0';

    return 0;
}

static int scsi_read_capacity(usb_storage_dev_t *sdev, u64 *sector_count, u32 *sector_size)
{
    u8 cap_data[8];
    memset(cap_data, 0, sizeof(cap_data));
    u8 cdb[10] = { SCSI_READ_CAPACITY_10, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

    int rc = usb_storage_scsi_command(sdev, cdb, sizeof(cdb), cap_data, sizeof(cap_data), true);
    if (rc < 0) return rc;

    u32 last_lba = ((u32)cap_data[0] << 24) | ((u32)cap_data[1] << 16) |
                   ((u32)cap_data[2] << 8)  | ((u32)cap_data[3]);
    u32 block_len = ((u32)cap_data[4] << 24) | ((u32)cap_data[5] << 16) |
                    ((u32)cap_data[6] << 8)  | ((u32)cap_data[7]);

    if (block_len == 0 || block_len > 4096) block_len = 512;

    *sector_count = (u64)last_lba + 1;
    *sector_size  = block_len;
    return 0;
}

/* ── Block Operations ─────────────────────────────────────────────────────── */

static s64 usb_storage_read_sectors(block_dev_t *dev, u64 lba, u32 count, void *buf)
{
    usb_storage_dev_t *sdev = (usb_storage_dev_t *)dev->driver_data;
    if (!sdev || !sdev->ready || !buf || count == 0) return -(s64)EINVAL;

    irqflags_t flags = spinlock_lock_irqsave(&sdev->lock);

    u32 bytes = count * dev->sector_size;
    u8 cdb[10];
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = SCSI_READ_10;
    cdb[2] = (u8)((lba >> 24) & 0xFF);
    cdb[3] = (u8)((lba >> 16) & 0xFF);
    cdb[4] = (u8)((lba >> 8) & 0xFF);
    cdb[5] = (u8)(lba & 0xFF);
    cdb[7] = (u8)((count >> 8) & 0xFF);
    cdb[8] = (u8)(count & 0xFF);

    int rc = usb_storage_scsi_command(sdev, cdb, sizeof(cdb), buf, bytes, true);
    spinlock_unlock_irqrestore(&sdev->lock, flags);

    if (rc < 0) return rc;
    return (s64)count;
}

static s64 usb_storage_write_sectors(block_dev_t *dev, u64 lba, u32 count, const void *buf)
{
    usb_storage_dev_t *sdev = (usb_storage_dev_t *)dev->driver_data;
    if (!sdev || !sdev->ready || !buf || count == 0) return -(s64)EINVAL;

    irqflags_t flags = spinlock_lock_irqsave(&sdev->lock);

    u32 bytes = count * dev->sector_size;
    u8 cdb[10];
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = SCSI_WRITE_10;
    cdb[2] = (u8)((lba >> 24) & 0xFF);
    cdb[3] = (u8)((lba >> 16) & 0xFF);
    cdb[4] = (u8)((lba >> 8) & 0xFF);
    cdb[5] = (u8)(lba & 0xFF);
    cdb[7] = (u8)((count >> 8) & 0xFF);
    cdb[8] = (u8)(count & 0xFF);

    int rc = usb_storage_scsi_command(sdev, cdb, sizeof(cdb), (void *)buf, bytes, false);
    spinlock_unlock_irqrestore(&sdev->lock, flags);

    if (rc < 0) return rc;
    return (s64)count;
}

static s64 usb_storage_flush(block_dev_t *dev)
{
    usb_storage_dev_t *sdev = (usb_storage_dev_t *)dev->driver_data;
    if (!sdev || !sdev->ready) return -(s64)ENODEV;

    irqflags_t flags = spinlock_lock_irqsave(&sdev->lock);
    u8 cdb[10];
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = SCSI_SYNCHRONIZE_CACHE_10;

    int rc = usb_storage_scsi_command(sdev, cdb, sizeof(cdb), NULL, 0, false);
    spinlock_unlock_irqrestore(&sdev->lock, flags);
    return rc < 0 ? rc : 0;
}

/* ── USB Class Driver Callbacks ───────────────────────────────────────────── */

static int usb_storage_probe(usb_device_t *dev,
                             const usb_interface_descriptor_t *intf,
                             const u8 *cfg, u16 cfg_len, void **priv)
{
    /* Only accept Mass Storage with Bulk-Only Transport */
    if (intf->bInterfaceClass != USB_CLASS_MASS_STORAGE) return -ENODEV;
    if (intf->bInterfaceProtocol != USB_PROTO_BULK_ONLY) return -ENODEV;

    /* Accept SCSI (0x06), SFF-8070i (0x05), UFI (0x04), ATAPI (0x02), RBC (0x01) */
    if (intf->bInterfaceSubClass != USB_SUBCLASS_SCSI &&
        intf->bInterfaceSubClass != USB_SUBCLASS_RBC &&
        intf->bInterfaceSubClass != USB_SUBCLASS_ATAPI &&
        intf->bInterfaceSubClass != USB_SUBCLASS_UFI &&
        intf->bInterfaceSubClass != USB_SUBCLASS_SFF8070I) {
        return -ENODEV;
    }

    /* Locate Bulk-IN and Bulk-OUT endpoints */
    const u8 *p = (const u8 *)intf + intf->bLength;
    const u8 *end = cfg + cfg_len;
    const usb_endpoint_descriptor_t *ep_in = NULL;
    const usb_endpoint_descriptor_t *ep_out = NULL;

    while (p + 2 <= end && p[0] >= 2 && p + p[0] <= end) {
        if (p[1] == USB_DESC_INTERFACE) break; /* Next interface descriptor reached */
        if (p[1] == USB_DESC_ENDPOINT && p[0] >= 7) {
            const usb_endpoint_descriptor_t *ep = (const usb_endpoint_descriptor_t *)p;
            if ((ep->bmAttributes & USB_ENDPOINT_XFER_MASK) == USB_ENDPOINT_XFER_BULK) {
                if (ep->bEndpointAddress & USB_ENDPOINT_DIR_IN) {
                    if (!ep_in) ep_in = ep;
                } else {
                    if (!ep_out) ep_out = ep;
                }
            }
        }
        p += p[0];
    }

    if (!ep_in || !ep_out) {
        pr_debug("[USB-STORAGE] Missing Bulk-IN or Bulk-OUT endpoint on %04x:%04x\n",
                 dev->vendor_id, dev->product_id);
        return -ENODEV;
    }

    usb_storage_dev_t *sdev = (usb_storage_dev_t *)kzalloc(sizeof(usb_storage_dev_t));
    if (!sdev) return -ENOMEM;

    sdev->udev      = dev;
    sdev->iface_num = intf->bInterfaceNumber;
    sdev->ep_in     = ep_in;
    sdev->ep_out    = ep_out;
    sdev->tag       = 0x1000;
    spinlock_init(&sdev->lock);

    /* Query Max LUN */
    sdev->max_lun = usb_storage_get_max_lun(sdev);

    /* Inquiry to identify device */
    if (scsi_inquiry(sdev) < 0) {
        pr_debug("[USB-STORAGE] SCSI INQUIRY failed on %04x:%04x\n",
                 dev->vendor_id, dev->product_id);
        kfree(sdev);
        return -EIO;
    }

    /* Wait for unit to be ready (up to 5 retries with request sense) */
    bool ready = false;
    for (int retry = 0; retry < 5; retry++) {
        if (scsi_test_unit_ready(sdev) == 0) {
            ready = true;
            break;
        }
        u8 sense[18];
        scsi_request_sense(sdev, sense, sizeof(sense));
        for (volatile int i = 0; i < 500000; i++) cpu_pause();
    }

    if (!ready) {
        pr_debug("[USB-STORAGE] Device %s %s not ready\n", sdev->vendor, sdev->product);
        kfree(sdev);
        return -EIO;
    }

    /* Read Capacity */
    u64 sector_count = 0;
    u32 sector_size = 512;
    if (scsi_read_capacity(sdev, &sector_count, &sector_size) < 0 || sector_count == 0) {
        pr_debug("[USB-STORAGE] READ CAPACITY failed\n");
        kfree(sdev);
        return -EIO;
    }

    sdev->ready = true;

    /* Setup block device */
    sdev->bops.read_sectors  = usb_storage_read_sectors;
    sdev->bops.write_sectors = usb_storage_write_sectors;
    sdev->bops.flush         = usb_storage_flush;
    sdev->bops.trim          = NULL;

    /* Assign drive name: sda, sdb, sdc, etc. */
    char disk_letter = (char)('a' + (g_usb_disk_index % 26));
    snprintf(sdev->bdev.name, sizeof(sdev->bdev.name), "sd%c", disk_letter);
    g_usb_disk_index++;

    sdev->bdev.sector_size      = sector_size;
    sdev->bdev.phys_sector_size = sector_size;
    sdev->bdev.sector_count     = sector_count;
    sdev->bdev.flags            = BLKDEV_REMOVABLE;
    sdev->bdev.ops              = &sdev->bops;
    sdev->bdev.driver_data      = sdev;

    s64 blk_err = block_dev_register(&sdev->bdev);
    if (blk_err < 0) {
        pr_debug("[USB-STORAGE] Failed to register block device /dev/%s: %lld\n",
                 sdev->bdev.name, (long long)blk_err);
        kfree(sdev);
        return (int)blk_err;
    }

    u64 mib = (sector_count * sector_size) / (1024 * 1024);
    pr_debug("[USB-STORAGE] USB Drive '%s %s' (%llu MiB, %llu sectors x %u B) -> /dev/%s\n",
             sdev->vendor, sdev->product, (unsigned long long)mib,
             (unsigned long long)sector_count, sector_size, sdev->bdev.name);

    *priv = sdev;
    return 0;
}

static void usb_storage_disconnect(usb_device_t *dev, void *priv)
{
    (void)dev;
    usb_storage_dev_t *sdev = (usb_storage_dev_t *)priv;
    if (!sdev) return;

    pr_debug("[USB-STORAGE] Disconnecting /dev/%s\n", sdev->bdev.name);
    sdev->ready = false;

    block_drop_partitions(&sdev->bdev);
    block_dev_unregister(&sdev->bdev);
    kfree(sdev);
}

const usb_class_driver_t usb_storage_driver = {
    .name       = "usb-storage",
    .probe      = usb_storage_probe,
    .disconnect = usb_storage_disconnect,
    .poll       = NULL,
};
