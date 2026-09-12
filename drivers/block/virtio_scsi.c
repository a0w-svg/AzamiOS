/* ============================================================================
 * AzamiOS — VirtIO SCSI Host Driver Implementation
 * File: drivers/block/virtio_scsi.c
 * ============================================================================ */

#define DEBUG 1
#include <azami/debug.h>
#include "virtio_scsi.h"
#include "../base/pci_bus.h"
#include "../../kernel/mm/kmalloc.h"
#include "../../arch/x86_64/mm/vmm.h"
#include "../../arch/x86_64/cpu/hwaccel.h"
#include "../../kernel/lib/string.h"

/* One global HBA instance — a second virtio-scsi-pci controller is refused
 * in probe() rather than silently reinitializing the transport and disks[]
 * array out from under the first, same as every other single-instance
 * driver converted in this pass. */
static virtio_scsi_dev_t g_vscsi;

/* ── Wire-level command submission ──────────────────────────────────────── */

/* Encode a single-level LUN per virtio-scsi §5.6.6.1 / SAM: byte 0 = 0x01,
 * byte 1 = target, bytes 2-3 = 0x40 | (lun >> 8) and (lun & 0xff), rest 0. */
static void vscsi_encode_lun(u8 out[8], u8 target, u16 lun)
{
    memset(out, 0, 8);
    out[0] = 1;
    out[1] = target;
    out[2] = (u8)(0x40 | ((lun >> 8) & 0x3F));
    out[3] = (u8)(lun & 0xFF);
}

/* Submit one request/response pair (plus optional data-out and data-in
 * buffers) on the request queue and block until the device retires it.
 *
 * Plain spinlock, not _irqsave, and a bounded poll: see virtio_gpu_submit()
 * in drivers/video/virtio_gpu.c for why — this driver was written after that
 * fix, so it never carried the interrupts-off busy-wait bug the others did.
 */
static int vscsi_submit(virtio_scsi_dev_t *dev,
                        struct virtio_scsi_cmd_req *req,
                        const void *out_buf, u32 out_len,
                        struct virtio_scsi_cmd_resp *resp,
                        void *in_buf, u32 in_len)
{
    phys_addr_t addrs[4];
    u32         lens[4];
    bool        is_write[4];
    u32         n = 0;

    addrs[n] = vmm_translate(vmm_kernel_space(), (virt_addr_t)req);
    lens[n]  = sizeof(*req);
    is_write[n] = false;
    n++;

    if (out_buf && out_len) {
        addrs[n] = vmm_translate(vmm_kernel_space(), (virt_addr_t)out_buf);
        lens[n]  = out_len;
        is_write[n] = false;
        n++;
    }

    addrs[n] = vmm_translate(vmm_kernel_space(), (virt_addr_t)resp);
    lens[n]  = sizeof(*resp);
    is_write[n] = true;
    n++;

    if (in_buf && in_len) {
        addrs[n] = vmm_translate(vmm_kernel_space(), (virt_addr_t)in_buf);
        lens[n]  = in_len;
        is_write[n] = true;
        n++;
    }

    spinlock_lock(&dev->lock);

    if (virtqueue_add_chain(dev->request_vq, addrs, lens, is_write, n, (void *)1) < 0) {
        spinlock_unlock(&dev->lock);
        return -EIO;
    }

    virtqueue_kick(dev->request_vq);
    virtio_pci_notify(&dev->vpci, 2, dev->request_vq);

    void *cookie = NULL;
    u64 spins = 0;
    const u64 SPIN_LIMIT = 200000000ULL;
    while (!cookie) {
        cookie = virtqueue_get_used(dev->request_vq, NULL);
        if (cookie) break;
        if (++spins >= SPIN_LIMIT) {
            spinlock_unlock(&dev->lock);
            pr_debug("[VIRTIO-SCSI] request timed out\n");
            return -EIO;
        }
        hw_spin_wait((u32)spins);
    }

    spinlock_unlock(&dev->lock);

    if (resp->response != VIRTIO_SCSI_S_OK) return -EIO;
    return 0;
}

/* status == 0 (GOOD) after a successful transport round trip. */
#define SCSI_STATUS_CHECK_CONDITION 0x02
#define SCSI_SENSE_KEY_UNIT_ATTENTION 0x06

static int vscsi_cmd(virtio_scsi_dev_t *dev, u8 target,
                     const u8 *cdb, u8 cdb_len,
                     const void *out_buf, u32 out_len,
                     void *in_buf, u32 in_len, u8 *out_status)
{
    struct virtio_scsi_cmd_req req;
    struct virtio_scsi_cmd_resp resp;

    /* A target reports UNIT ATTENTION (sense key 6) on the first command
     * after it powers on or is reset, purely to tell the initiator "state
     * may have changed since you last looked" — it is not a real failure,
     * and a well-behaved initiator just reissues the command once the
     * condition has been observed. Retrying here (instead of at every call
     * site) is what lets the very first READ CAPACITY/READ/WRITE this
     * driver ever sends to a freshly attached disk actually succeed. */
    for (int attempt = 0; attempt < 3; attempt++) {
        memset(&req, 0, sizeof(req));
        vscsi_encode_lun(req.lun, target, 0);
        memcpy(req.cdb, cdb, cdb_len);

        memset(&resp, 0, sizeof(resp));

        int ret = vscsi_submit(dev, &req, out_buf, out_len, &resp, in_buf, in_len);
        if (ret < 0) return ret;

        if (resp.status == SCSI_STATUS_CHECK_CONDITION &&
            resp.sense_len >= 3 && resp.sense[2] == SCSI_SENSE_KEY_UNIT_ATTENTION) {
            continue;
        }

        if (out_status) *out_status = resp.status;
        return 0;
    }

    /* Still seeing UNIT ATTENTION after every retry: report it as-is rather
     * than looping forever. */
    if (out_status) *out_status = resp.status;
    return 0;
}

/* ── SCSI commands this driver actually issues ────────────────────────────
 * Direct-access disks only: INQUIRY, READ CAPACITY (10), READ (10), WRITE
 * (10). Nothing here talks to a tape, changer, or anything CDB-size-32
 * exotic — that is why VIRTIO_SCSI_CDB_SIZE only has to be 32, not whatever
 * the device's config space advertises. */

#define SCSI_STATUS_GOOD 0x00

static int vscsi_inquiry(virtio_scsi_dev_t *dev, u8 target, u8 *buf36)
{
    u8 cdb[16] = {0};
    cdb[0] = 0x12;               /* INQUIRY */
    cdb[3] = 0;                  /* allocation length high byte */
    cdb[4] = 36;                 /* allocation length low byte  */

    u8 status = 0xFF;
    if (vscsi_cmd(dev, target, cdb, sizeof(cdb), NULL, 0, buf36, 36, &status) < 0)
        return -1;
    return status == SCSI_STATUS_GOOD ? 0 : -1;
}

static int vscsi_read_capacity10(virtio_scsi_dev_t *dev, u8 target,
                                 u64 *out_sectors, u32 *out_block_size)
{
    u8 cdb[16] = {0};
    cdb[0] = 0x25;               /* READ CAPACITY (10) */
    u8 data[8];

    u8 status = 0xFF;
    if (vscsi_cmd(dev, target, cdb, sizeof(cdb), NULL, 0, data, sizeof(data), &status) < 0)
        return -1;
    if (status != SCSI_STATUS_GOOD) return -1;

    u32 last_lba = ((u32)data[0] << 24) | ((u32)data[1] << 16) |
                   ((u32)data[2] << 8)  |  (u32)data[3];
    u32 block_size = ((u32)data[4] << 24) | ((u32)data[5] << 16) |
                      ((u32)data[6] << 8)  |  (u32)data[7];

    *out_sectors    = (u64)last_lba + 1;
    *out_block_size = block_size ? block_size : 512;
    return 0;
}

/* READ/WRITE (10) address 32-bit LBAs and a 16-bit block count each, so a
 * caller asking for more than 65535 sectors — or an LBA that would need more
 * than 32 bits, which this driver does not support — is chunked/rejected
 * here rather than silently truncating the CDB fields. */
#define VSCSI_MAX_BLOCKS_PER_CMD 65535u

static s64 vscsi_transfer10(virtio_scsi_disk_t *disk, u8 opcode, u64 lba,
                            u32 count, void *buf, bool is_read)
{
    if (lba > 0xFFFFFFFFULL || lba + count > 0xFFFFFFFFULL) return -EINVAL;

    u32 done = 0;
    while (done < count) {
        u32 chunk = count - done;
        if (chunk > VSCSI_MAX_BLOCKS_PER_CMD) chunk = VSCSI_MAX_BLOCKS_PER_CMD;

        u8 cdb[16] = {0};
        u32 clba = (u32)(lba + done);
        cdb[0] = opcode;
        cdb[2] = (u8)(clba >> 24);
        cdb[3] = (u8)(clba >> 16);
        cdb[4] = (u8)(clba >> 8);
        cdb[5] = (u8)clba;
        cdb[7] = (u8)(chunk >> 8);
        cdb[8] = (u8)chunk;

        u8 *cbuf = (u8 *)buf + (size_t)done * disk->sector_size;
        u32 xfer_len = chunk * disk->sector_size;
        u8 status = 0xFF;

        int ret = is_read
            ? vscsi_cmd(disk->host, disk->target, cdb, sizeof(cdb), NULL, 0, cbuf, xfer_len, &status)
            : vscsi_cmd(disk->host, disk->target, cdb, sizeof(cdb), cbuf, xfer_len, NULL, 0, &status);
        if (ret < 0 || status != SCSI_STATUS_GOOD) return -EIO;

        done += chunk;
    }
    return (s64)count;
}

static s64 vscsi_read_sectors(struct block_dev *bdev, u64 lba, u32 count, void *buf)
{
    virtio_scsi_disk_t *disk = (virtio_scsi_disk_t *)bdev->driver_data;
    if (!disk || !buf || count == 0) return -EINVAL;
    return vscsi_transfer10(disk, 0x28 /* READ (10) */, lba, count, buf, true);
}

static s64 vscsi_write_sectors(struct block_dev *bdev, u64 lba, u32 count, const void *buf)
{
    virtio_scsi_disk_t *disk = (virtio_scsi_disk_t *)bdev->driver_data;
    if (!disk || !buf || count == 0) return -EINVAL;
    return vscsi_transfer10(disk, 0x2A /* WRITE (10) */, lba, count, (void *)buf, false);
}

static block_ops_t g_vscsi_block_ops = {
    .read_sectors  = vscsi_read_sectors,
    .write_sectors = vscsi_write_sectors,
};

/* ── Target scan ───────────────────────────────────────────────────────── */

static void vscsi_scan_targets(virtio_scsi_dev_t *dev, u16 max_target)
{
    u16 n = max_target + 1 < VIRTIO_SCSI_MAX_TARGETS ? (u16)(max_target + 1)
                                                       : VIRTIO_SCSI_MAX_TARGETS;

    for (u16 t = 0; t < n; t++) {
        u8 inquiry[36];
        if (vscsi_inquiry(dev, (u8)t, inquiry) < 0) continue;

        /* Peripheral qualifier/device type byte: 0x00 low nibble is a
         * direct-access block device; anything else (no LUN, a CD-ROM, a
         * changer, ...) is out of scope for this block driver. */
        if ((inquiry[0] & 0x1F) != 0x00) continue;

        u64 sectors = 0;
        u32 block_size = 512;
        if (vscsi_read_capacity10(dev, (u8)t, &sectors, &block_size) < 0) continue;
        if (sectors == 0) continue;

        virtio_scsi_disk_t *disk = &dev->disks[dev->ndisks];
        memset(disk, 0, sizeof(*disk));
        disk->host             = dev;
        disk->target           = (u8)t;
        disk->capacity_sectors = sectors;
        disk->sector_size      = block_size;

        char name[16];
        strncpy(name, "vscsi", sizeof(name) - 1);
        size_t len = strlen(name);
        name[len]   = (char)('0' + dev->ndisks);
        name[len+1] = '\0';

        strncpy(disk->bdev.name, name, sizeof(disk->bdev.name) - 1);
        disk->bdev.sector_size  = block_size;
        disk->bdev.sector_count = sectors;
        disk->bdev.ops          = &g_vscsi_block_ops;
        disk->bdev.driver_data  = disk;
        block_dev_register(&disk->bdev);

        char vendor[9], product[17];
        memcpy(vendor, &inquiry[8], 8);   vendor[8]  = '\0';
        memcpy(product, &inquiry[16], 16); product[16] = '\0';
        pr_debug("[VIRTIO-SCSI] target %u: '%s' registered as '%s' "
                 "(%llu sectors, %u B, '%.8s %.16s')\n",
                 t, name, name, (unsigned long long)sectors, block_size,
                 vendor, product);

        /* TEMP-SELFTEST: same idea as the one in drivers/block/nvme.c — round
         * a write through the actual WRITE(10)/READ(10) wire path and back,
         * far enough into the disk (sector 100) to stay clear of any real
         * boot sector or partition table already on it. */
        {
            u8 wbuf[4096], rbuf[4096];
            for (u32 i = 0; i < sizeof(wbuf); i++) wbuf[i] = (u8)(i * 5 + 7);
            u32 nsec = sizeof(wbuf) / block_size;
            if (nsec == 0) nsec = 1;

            s64 w = vscsi_write_sectors(&disk->bdev, 100, nsec, wbuf);
            memset(rbuf, 0, sizeof(rbuf));
            s64 r = vscsi_read_sectors(&disk->bdev, 100, nsec, rbuf);
            int bad = -1;
            u32 cmp_len = nsec * block_size;
            if (cmp_len > sizeof(wbuf)) cmp_len = sizeof(wbuf);
            for (u32 i = 0; i < cmp_len; i++)
                if (wbuf[i] != rbuf[i]) { bad = (int)i; break; }
            pr_debug("[VIRTIO-SCSI-SELFTEST] target %u: %u sectors: w=%lld r=%lld mismatch@%d -> %s\n",
                     t, nsec, (long long)w, (long long)r, bad,
                     (w == (s64)nsec && r == (s64)nsec && bad < 0) ? "PASS" : "FAIL");
        }

        dev->ndisks++;
        if (dev->ndisks >= VIRTIO_SCSI_MAX_TARGETS) break;
    }
}

/* ── PCI binding ───────────────────────────────────────────────────────── */

static int virtio_scsi_probe(dm_device_t *dm, const pci_device_id_t *id)
{
    (void)id;
    if (g_vscsi.active) return -EBUSY;

    pci_device_info_t *info = to_pci_info(dm);
    if (!info) return -ENODEV;

    pr_debug("[VIRTIO-SCSI] Found VirtIO SCSI host at PCI %02x:%02x.%x\n",
             info->bus, info->slot, info->func);

    memset(&g_vscsi, 0, sizeof(g_vscsi));
    spinlock_init(&g_vscsi.lock);

    if (virtio_pci_init_device(dm->hal, &g_vscsi.vpci) < 0) {
        pr_debug("[VIRTIO-SCSI] Failed to initialize VirtIO PCI transport\n");
        return -ENODEV;
    }

    virtio_pci_set_status(&g_vscsi.vpci, 0);
    virtio_pci_set_status(&g_vscsi.vpci,
                          virtio_pci_get_status(&g_vscsi.vpci) | VIRTIO_CONFIG_S_ACKNOWLEDGE | VIRTIO_CONFIG_S_DRIVER);

    if (!virtio_pci_negotiate_features(&g_vscsi.vpci, 0)) {
        pr_debug("[VIRTIO-SCSI] Failed to negotiate features\n");
        virtio_pci_set_status(&g_vscsi.vpci, VIRTIO_CONFIG_S_FAILED);
        return -ENODEV;
    }

    /* control(0), event(1), request(2) — this driver only ever touches the
     * request queue, but all three have to be set up (or the device never
     * reaches DRIVER_OK-ready state on some hosts) and left unused. */
    g_vscsi.control_vq = virtio_pci_setup_queue(&g_vscsi.vpci, 0);
    g_vscsi.event_vq   = virtio_pci_setup_queue(&g_vscsi.vpci, 1);
    g_vscsi.request_vq = virtio_pci_setup_queue(&g_vscsi.vpci, 2);
    if (!g_vscsi.control_vq || !g_vscsi.event_vq || !g_vscsi.request_vq) {
        pr_debug("[VIRTIO-SCSI] Failed to set up virtqueues\n");
        virtio_pci_set_status(&g_vscsi.vpci, VIRTIO_CONFIG_S_FAILED);
        return -ENODEV;
    }

    virtio_pci_set_status(&g_vscsi.vpci,
                          virtio_pci_get_status(&g_vscsi.vpci) | VIRTIO_CONFIG_S_DRIVER_OK);
    g_vscsi.active = true;

    u16 max_target = 0;
    if (g_vscsi.vpci.device_cfg) {
        struct virtio_scsi_config cfg;
        memcpy(&cfg, (const void *)g_vscsi.vpci.device_cfg, sizeof(cfg));
        max_target = cfg.max_target;
        pr_debug("[VIRTIO-SCSI] num_queues=%u max_target=%u max_lun=%u cdb_size=%u sense_size=%u\n",
                 cfg.num_queues, cfg.max_target, cfg.max_lun, cfg.cdb_size, cfg.sense_size);
    }
    if (max_target == 0 || max_target >= VIRTIO_SCSI_MAX_TARGETS)
        max_target = VIRTIO_SCSI_MAX_TARGETS - 1;

    vscsi_scan_targets(&g_vscsi, max_target);
    if (g_vscsi.ndisks == 0)
        pr_debug("[VIRTIO-SCSI] no direct-access disks found behind this host\n");

    dm_set_drvdata(dm, &g_vscsi);
    return 0;
}

static void virtio_scsi_remove(dm_device_t *dm)
{
    (void)dm;
    if (!g_vscsi.active) return;
    virtio_pci_set_status(&g_vscsi.vpci, 0);
    g_vscsi.active = false;
}

/* 0x1004: legacy/transitional (this is what QEMU's virtio-scsi-pci actually
 * advertises by default); 0x1048 = 0x1040 + VIRTIO_ID_SCSI(8), the
 * modern-only id a strict VIRTIO_F_VERSION_1 host could use instead. */
static const pci_device_id_t virtio_scsi_pci_ids[] = {
    { PCI_DEVICE(0x1AF4, 0x1004) },
    { PCI_DEVICE(0x1AF4, 0x1048) },
    { 0 }
};

static pci_driver_t virtio_scsi_pci_driver = {
    .drv      = { .name = "virtio_scsi" },
    .id_table = virtio_scsi_pci_ids,
    .probe    = virtio_scsi_probe,
    .remove   = virtio_scsi_remove,
};

void virtio_scsi_init(void)
{
    pci_driver_register(&virtio_scsi_pci_driver);
}
