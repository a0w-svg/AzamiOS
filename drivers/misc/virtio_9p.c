/* ============================================================================
 * AzamiOS — VirtIO 9P / VirtFS Host-Guest Shared Folder Driver
 * File: drivers/misc/virtio_9p.c
 * ============================================================================ */

#define DEBUG 1
#include "../../include/azami/debug.h"
#include "virtio_9p.h"
#include "../../fs/vfs.h"
#include "../../kernel/uaccess.h"
#include "../../kernel/lib/string.h"

static virtio_9p_dev_t g_v9p_dev;

extern int devfs_register_device(const char *name, file_operations_t *fops, void *private_data);

int virtio_9p_get_tag(char *buf, size_t maxlen)
{
    if (!buf || maxlen == 0 || !g_v9p_dev.active) return -ENODEV;
    strncpy(buf, g_v9p_dev.tag, maxlen - 1);
    buf[maxlen - 1] = '\0';
    return 0;
}

/* ── Character Device Operations for /dev/virtfs and /dev/9p0 ────────────── */

static s64 dev_virtfs_read(file_t *filp, void *buf, size_t len, u64 *offset)
{
    (void)filp;
    if (!buf || len == 0 || !g_v9p_dev.active) return 0;
    if (*offset > 0) return 0;

    char status[256];
    int n = scnprintf(status, sizeof(status),
                      "VirtIO-9P Shared Filesystem Channel (/dev/virtfs)\n"
                      "Mount Tag: '%s' (tag_len=%u)\n"
                      "Status: ONLINE\n",
                      g_v9p_dev.tag, g_v9p_dev.tag_len);

    if (len > (size_t)n) len = (size_t)n;
    memcpy(buf, status, len);
    *offset += len;
    return (s64)len;
}

static s64 dev_virtfs_ioctl(file_t *filp, u32 cmd, u64 arg)
{
    (void)filp;
    if (!g_v9p_dev.active) return -ENODEV;

    switch (cmd) {
    case 0x9001: /* VIRTFS_IOC_GET_TAG */
        if (!arg) return -EINVAL;
        if (copy_to_user((void *)arg, g_v9p_dev.tag, g_v9p_dev.tag_len + 1) != 0) {
            return -EFAULT;
        }
        return 0;
    default:
        return -EINVAL;
    }
}

static file_operations_t g_virtfs_fops = {
    .read  = dev_virtfs_read,
    .write = NULL,
    .ioctl = dev_virtfs_ioctl,
};

/* ── PCI Probe / Driver Model ────────────────────────────────────────────── */

static int virtio_9p_probe(dm_device_t *dm, const pci_device_id_t *id)
{
    (void)id;
    if (g_v9p_dev.active) return -EBUSY;

    pci_device_info_t *info = to_pci_info(dm);
    if (!info) return -ENODEV;

    if (virtio_pci_init_device(dm->hal, &g_v9p_dev.vpci) < 0) {
        return -ENODEV;
    }

    virtio_pci_set_status(&g_v9p_dev.vpci, 0); /* Reset */
    virtio_pci_set_status(&g_v9p_dev.vpci,
                          virtio_pci_get_status(&g_v9p_dev.vpci) |
                          VIRTIO_CONFIG_S_ACKNOWLEDGE | VIRTIO_CONFIG_S_DRIVER);

    if (!virtio_pci_negotiate_features(&g_v9p_dev.vpci, 0)) {
        virtio_pci_set_status(&g_v9p_dev.vpci, VIRTIO_CONFIG_S_FAILED);
        return -1;
    }

    /* Allocate request VirtQueue (Queue 0) */
    g_v9p_dev.vq = virtio_pci_setup_queue(&g_v9p_dev.vpci, 0);
    if (!g_v9p_dev.vq) {
        virtio_pci_set_status(&g_v9p_dev.vpci, VIRTIO_CONFIG_S_FAILED);
        return -ENOMEM;
    }

    /* Read mount tag from device configuration space */
    memset(g_v9p_dev.tag, 0, sizeof(g_v9p_dev.tag));
    g_v9p_dev.tag_len = 0;

    if (g_v9p_dev.vpci.device_cfg) {
        volatile u16 *tlen_ptr = (volatile u16 *)g_v9p_dev.vpci.device_cfg;
        u16 tlen = *tlen_ptr;
        if (tlen > 0 && tlen < VIRTIO_9P_TAG_MAX - 1) {
            g_v9p_dev.tag_len = tlen;
            volatile u8 *tag_ptr = (volatile u8 *)(g_v9p_dev.vpci.device_cfg + 2);
            for (u16 i = 0; i < tlen; i++) {
                g_v9p_dev.tag[i] = tag_ptr[i];
            }
            g_v9p_dev.tag[tlen] = '\0';
        }
    }

    if (g_v9p_dev.tag_len == 0) {
        /* Default fallback tag if unpopulated in config */
        strncpy(g_v9p_dev.tag, "hostshare", sizeof(g_v9p_dev.tag) - 1);
        g_v9p_dev.tag_len = (u16)strlen(g_v9p_dev.tag);
    }

    virtio_pci_set_status(&g_v9p_dev.vpci,
                          virtio_pci_get_status(&g_v9p_dev.vpci) | VIRTIO_CONFIG_S_DRIVER_OK);

    g_v9p_dev.active = true;

    devfs_register_device("virtfs", &g_virtfs_fops, &g_v9p_dev);
    devfs_register_device("9p0", &g_virtfs_fops, &g_v9p_dev);

    pr_debug("[VIRTFS] VirtIO-9P filesystem channel online (tag='%s', /dev/virtfs, /dev/9p0)\n",
             g_v9p_dev.tag);
    return 0;
}

static void virtio_9p_remove(dm_device_t *dm)
{
    (void)dm;
    if (!g_v9p_dev.active) return;
    virtio_pci_set_status(&g_v9p_dev.vpci, 0);
    g_v9p_dev.active = false;
}

static const pci_device_id_t g_virtio_9p_pci_ids[] = {
    { PCI_DEVICE(0x1AF4, VIRTIO_9P_TRANSITIONAL_ID) },
    { PCI_DEVICE(0x1AF4, VIRTIO_9P_MODERN_ID) },
    { 0 }
};

static pci_driver_t g_virtio_9p_pci_driver = {
    .drv = {
        .name = "virtio_9p",
    },
    .id_table = g_virtio_9p_pci_ids,
    .probe    = virtio_9p_probe,
    .remove   = virtio_9p_remove,
};

int virtio_9p_init(void)
{
    return pci_driver_register(&g_virtio_9p_pci_driver);
}
