/* ============================================================================
 * AzamiOS — SD / MMC Host Controller Interface (SDHCI) Driver
 * File: drivers/block/sdhci.c
 *
 * Implements SDHCI Standard Spec v1.0 - v4.0 for PCI SD/MMC host controllers:
 *   - Realtek RTS5209, RTS5227, RTS5229, RTS5287, RTS5289
 *   - Intel Sunrise Point, Cannon Point, Tiger Lake, Alder Lake SDHCI
 *   - O2Micro, Ricoh, TI, JMicron PCI SD card controllers
 *   - QEMU / VirtualBox PCI Class 0x08 Subclass 0x05
 * ============================================================================ */

#define DEBUG 1
#include "../../include/azami/debug.h"
#include "sdhci.h"
#include "../base/pci_bus.h"
#include "../../hal/pci.h"
#include "../../kernel/mm/kmalloc.h"
#include "../../kernel/lib/string.h"
#include "../../arch/x86_64/cpu/spinlock.h"

#define SDHCI_MAX_HOSTS 4
static sdhci_host_t g_sdhci_hosts[SDHCI_MAX_HOSTS];
static u32          g_sdhci_host_count = 0;

/* ── MMIO Accessors ───────────────────────────────────────────────────────── */

static inline u8 sdhci_read8(sdhci_host_t *h, u32 reg)
{
    return *(volatile u8 *)(h->mmio_base + reg);
}

static inline u16 sdhci_read16(sdhci_host_t *h, u32 reg)
{
    return *(volatile u16 *)(h->mmio_base + reg);
}

static inline u32 sdhci_read32(sdhci_host_t *h, u32 reg)
{
    return *(volatile u32 *)(h->mmio_base + reg);
}

static inline void sdhci_write8(sdhci_host_t *h, u32 reg, u8 val)
{
    *(volatile u8 *)(h->mmio_base + reg) = val;
}

static inline void sdhci_write16(sdhci_host_t *h, u32 reg, u16 val)
{
    *(volatile u16 *)(h->mmio_base + reg) = val;
}

static inline void sdhci_write32(sdhci_host_t *h, u32 reg, u32 val)
{
    *(volatile u32 *)(h->mmio_base + reg) = val;
}

/* ── Controller Reset & Clock ─────────────────────────────────────────────── */

static int sdhci_reset(sdhci_host_t *h, u8 mask)
{
    sdhci_write8(h, SDHCI_SOFTWARE_RESET, mask);
    for (u32 i = 0; i < 100000; i++) {
        if ((sdhci_read8(h, SDHCI_SOFTWARE_RESET) & mask) == 0)
            return 0;
        __asm__ volatile("pause");
    }
    return -ETIMEDOUT;
}

static int sdhci_set_clock(sdhci_host_t *h, u16 div)
{
    /* Stop SD clock */
    u16 clk = sdhci_read16(h, SDHCI_CLOCK_CONTROL);
    clk &= ~SDHCI_CLOCK_SD_EN;
    sdhci_write16(h, SDHCI_CLOCK_CONTROL, clk);

    /* Set divisor and enable internal oscillator */
    clk = (div << 8) | SDHCI_CLOCK_INTERNAL_EN;
    sdhci_write16(h, SDHCI_CLOCK_CONTROL, clk);

    /* Wait for oscillator stability */
    bool stable = false;
    for (u32 i = 0; i < 100000; i++) {
        if (sdhci_read16(h, SDHCI_CLOCK_CONTROL) & SDHCI_CLOCK_STABLE) {
            stable = true;
            break;
        }
        __asm__ volatile("pause");
    }
    if (!stable) return -ETIMEDOUT;

    /* Enable SD Clock output */
    clk |= SDHCI_CLOCK_SD_EN;
    sdhci_write16(h, SDHCI_CLOCK_CONTROL, clk);
    return 0;
}

/* ── Command Execution ────────────────────────────────────────────────────── */

static int sdhci_send_cmd(sdhci_host_t *h, u8 cmd_idx, u32 arg, u16 transfer_mode,
                          u8 resp_type, bool data_present, u32 resp[4])
{
    /* Wait for CMD inhibit */
    for (u32 i = 0; i < 200000; i++) {
        u32 st = sdhci_read32(h, SDHCI_PRESENT_STATE);
        if (!(st & SDHCI_CMD_INHIBIT)) {
            if (!data_present || !(st & SDHCI_DATA_INHIBIT))
                break;
        }
        __asm__ volatile("pause");
    }

    /* Clear all pending interrupt status */
    sdhci_write16(h, SDHCI_INT_STATUS, 0xFFFF);
    sdhci_write16(h, SDHCI_ERR_INT_STATUS, 0xFFFF);

    sdhci_write32(h, SDHCI_ARGUMENT, arg);
    sdhci_write16(h, SDHCI_TRANSFER_MODE, transfer_mode);

    u16 cmd_flags = ((u16)cmd_idx << 8);
    if (resp_type == SDHCI_CMD_RESP_136) {
        cmd_flags |= SDHCI_CMD_RESP_136 | SDHCI_CMD_CRC_CHECK;
    } else if (resp_type == SDHCI_CMD_RESP_48) {
        cmd_flags |= SDHCI_CMD_RESP_48 | SDHCI_CMD_CRC_CHECK | SDHCI_CMD_INDEX_CHECK;
    } else if (resp_type == SDHCI_CMD_RESP_48_BUSY) {
        cmd_flags |= SDHCI_CMD_RESP_48_BUSY | SDHCI_CMD_CRC_CHECK | SDHCI_CMD_INDEX_CHECK;
    }
    if (data_present) cmd_flags |= SDHCI_CMD_DATA_PRESENT;

    sdhci_write16(h, SDHCI_COMMAND, cmd_flags);

    /* Poll for completion or error */
    for (u32 i = 0; i < 500000; i++) {
        u16 int_stat = sdhci_read16(h, SDHCI_INT_STATUS);
        if (int_stat & SDHCI_INT_ERROR) {
            u16 err = sdhci_read16(h, SDHCI_ERR_INT_STATUS);
            sdhci_write16(h, SDHCI_INT_STATUS, int_stat);
            sdhci_write16(h, SDHCI_ERR_INT_STATUS, err);
            return -EIO;
        }
        if (int_stat & SDHCI_INT_CMD_COMPLETE) {
            sdhci_write16(h, SDHCI_INT_STATUS, SDHCI_INT_CMD_COMPLETE);
            if (resp) {
                resp[0] = sdhci_read32(h, SDHCI_RESPONSE_0);
                if (resp_type == SDHCI_CMD_RESP_136) {
                    resp[1] = sdhci_read32(h, SDHCI_RESPONSE_1);
                    resp[2] = sdhci_read32(h, SDHCI_RESPONSE_2);
                    resp[3] = sdhci_read32(h, SDHCI_RESPONSE_3);
                }
            }
            return 0;
        }
        __asm__ volatile("pause");
    }

    return -ETIMEDOUT;
}

static int sdhci_app_cmd(sdhci_host_t *h, u8 cmd_idx, u32 arg, u8 resp_type, u32 resp[4])
{
    u32 app_resp[4];
    int rc = sdhci_send_cmd(h, MMC_APP_CMD, ((u32)h->rca) << 16, 0, SDHCI_CMD_RESP_48, false, app_resp);
    if (rc != 0) return rc;
    return sdhci_send_cmd(h, cmd_idx, arg, 0, resp_type, false, resp);
}

/* ── Card Initialization ──────────────────────────────────────────────────── */

static int sdhci_card_init(sdhci_host_t *h)
{
    u32 resp[4];

    /* 1. Software Reset All */
    if (sdhci_reset(h, SDHCI_RESET_ALL) != 0) {
        pr_debug("[SDHCI] Reset failed\n");
        return -EIO;
    }

    /* 2. Enable All Interrupt Status Signals */
    sdhci_write16(h, SDHCI_INT_ENABLE, 0xFFFF);
    sdhci_write16(h, SDHCI_ERR_INT_ENABLE, 0xFFFF);
    sdhci_write16(h, SDHCI_SIGNAL_ENABLE, 0);

    /* 3. Power Control: 3.3V & Power ON */
    sdhci_write8(h, SDHCI_POWER_CONTROL, SDHCI_POWER_330 | SDHCI_POWER_ON);

    /* 4. Set Initial Identification Clock (400 kHz) */
    sdhci_set_clock(h, 0x80);

    /* 5. Check if card is present */
    u32 pstate = sdhci_read32(h, SDHCI_PRESENT_STATE);
    if (!(pstate & SDHCI_CARD_PRESENT)) {
        pr_debug("[SDHCI] No SD card detected in slot\n");
        return -ENODEV;
    }
    h->card_inserted = true;

    /* 6. CMD0: Reset card to IDLE */
    sdhci_send_cmd(h, MMC_GO_IDLE_STATE, 0, 0, SDHCI_CMD_RESP_NONE, false, NULL);

    /* 7. CMD8: Send Interface Condition (2.7-3.6V, pattern 0xAA) */
    bool is_v2 = false;
    if (sdhci_send_cmd(h, MMC_SEND_IF_COND, 0x1AA, 0, SDHCI_CMD_RESP_48, false, resp) == 0) {
        if ((resp[0] & 0xFF) == 0xAA) is_v2 = true;
    }

    /* 8. ACMD41: Initialize and negotiate capacity */
    u32 op_arg = 0x00FF8000; /* 2.7-3.6V range */
    if (is_v2) op_arg |= (1U << 30); /* HCS (High Capacity Support) */

    bool ready = false;
    for (u32 attempt = 0; attempt < 200; attempt++) {
        if (sdhci_app_cmd(h, SD_APP_OP_COND, op_arg, SDHCI_CMD_RESP_48, resp) == 0) {
            if (resp[0] & (1U << 31)) { /* Card ready */
                ready = true;
                if (resp[0] & (1U << 30)) {
                    h->card_type = SD_TYPE_HIGH_CAPACITY; /* SDHC / SDXC */
                } else {
                    h->card_type = SD_TYPE_STANDARD;      /* SDSC */
                }
                break;
            }
        }
        for (volatile int d = 0; d < 10000; d++);
    }

    if (!ready) {
        pr_debug("[SDHCI] Card failed to power up (ACMD41 timeout)\n");
        return -ETIMEDOUT;
    }

    /* 9. CMD2: ALL_SEND_CID (Card identification) */
    sdhci_send_cmd(h, MMC_ALL_SEND_CID, 0, 0, SDHCI_CMD_RESP_136, false, resp);

    /* 10. CMD3: SEND_RELATIVE_ADDR */
    if (sdhci_send_cmd(h, MMC_SET_RELATIVE_ADDR, 0, 0, SDHCI_CMD_RESP_48, false, resp) != 0) {
        pr_debug("[SDHCI] Failed to get card RCA\n");
        return -EIO;
    }
    h->rca = (u16)(resp[0] >> 16);

    /* 11. CMD9: SEND_CSD (Card Specific Data) */
    if (sdhci_send_cmd(h, MMC_SEND_CSD, ((u32)h->rca) << 16, 0, SDHCI_CMD_RESP_136, false, resp) != 0) {
        pr_debug("[SDHCI] Failed to read CSD\n");
        return -EIO;
    }

    /* Calculate capacity */
    if (h->card_type == SD_TYPE_HIGH_CAPACITY) {
        /* CSD v2.0: C_SIZE is bits 48:69 in resp[1] and resp[2] */
        u32 c_size = ((resp[1] >> 16) & 0xFFFF) | ((resp[2] & 0x3F) << 16);
        h->sector_count = (u64)(c_size + 1) * 1024;
    } else {
        /* CSD v1.0: Standard 2GB or less */
        u32 c_size = ((resp[1] >> 30) & 0x3) | ((resp[2] & 0x3FF) << 2);
        u32 c_mult = (resp[1] >> 15) & 0x7;
        u32 read_bl_len = (resp[2] >> 16) & 0xF;
        h->sector_count = ((u64)(c_size + 1) << (c_mult + 2 + read_bl_len)) / 512;
    }
    h->sector_size = 512;
    h->capacity_bytes = h->sector_count * 512;

    /* 12. CMD7: Select Card */
    sdhci_send_cmd(h, MMC_SELECT_CARD, ((u32)h->rca) << 16, 0, SDHCI_CMD_RESP_48_BUSY, false, NULL);

    /* 13. Switch to 4-bit bus width */
    sdhci_app_cmd(h, SD_APP_SET_BUS_WIDTH, 2, SDHCI_CMD_RESP_48, NULL);
    u8 hc = sdhci_read8(h, SDHCI_HOST_CONTROL);
    hc |= 0x02; /* 4-bit mode */
    sdhci_write8(h, SDHCI_HOST_CONTROL, hc);

    /* 14. Switch to High-Speed Operational Clock (25 MHz) */
    sdhci_set_clock(h, 0x02);

    /* 15. CMD16: Set Block Length to 512 */
    sdhci_send_cmd(h, MMC_SET_BLOCKLEN, 512, 0, SDHCI_CMD_RESP_48, false, NULL);

    return 0;
}

/* ── Sector Read / Write (PIO Mode) ───────────────────────────────────────── */

static s64 sdhci_read_sectors(block_dev_t *bdev, u64 lba, u32 count, void *buf)
{
    sdhci_host_t *h = (sdhci_host_t *)bdev->driver_data;
    if (!h || !buf || count == 0) return -EINVAL;
    if (lba + count > h->sector_count) return -EINVAL;

    spinlock_lock(&h->lock);

    u8 *dst = (u8 *)buf;
    for (u32 s = 0; s < count; s++) {
        u64 sector = lba + s;
        u32 arg = (h->card_type == SD_TYPE_HIGH_CAPACITY) ? (u32)sector : (u32)(sector * 512);

        sdhci_write16(h, SDHCI_BLOCK_SIZE, 512);
        sdhci_write16(h, SDHCI_BLOCK_COUNT, 1);

        int rc = sdhci_send_cmd(h, MMC_READ_SINGLE_BLOCK, arg,
                                SDHCI_TRNS_READ | SDHCI_TRNS_BLK_CNT_EN,
                                SDHCI_CMD_RESP_48, true, NULL);
        if (rc != 0) {
            spinlock_unlock(&h->lock);
            return rc;
        }

        /* Wait for Buffer Read Ready */
        bool ready = false;
        for (u32 i = 0; i < 200000; i++) {
            if (sdhci_read16(h, SDHCI_INT_STATUS) & SDHCI_INT_BUF_READ_READY) {
                sdhci_write16(h, SDHCI_INT_STATUS, SDHCI_INT_BUF_READ_READY);
                ready = true;
                break;
            }
            __asm__ volatile("pause");
        }
        if (!ready) {
            spinlock_unlock(&h->lock);
            return -EIO;
        }

        /* Read 512 bytes (128 dwords) from buffer port */
        u32 *p = (u32 *)(dst + s * 512);
        for (int i = 0; i < 128; i++) {
            p[i] = sdhci_read32(h, SDHCI_BUFFER);
        }

        /* Wait for transfer complete */
        for (u32 i = 0; i < 100000; i++) {
            if (sdhci_read16(h, SDHCI_INT_STATUS) & SDHCI_INT_XFER_COMPLETE) {
                sdhci_write16(h, SDHCI_INT_STATUS, SDHCI_INT_XFER_COMPLETE);
                break;
            }
            __asm__ volatile("pause");
        }
    }

    spinlock_unlock(&h->lock);
    return (s64)count * 512;
}

static s64 sdhci_write_sectors(block_dev_t *bdev, u64 lba, u32 count, const void *buf)
{
    sdhci_host_t *h = (sdhci_host_t *)bdev->driver_data;
    if (!h || !buf || count == 0) return -EINVAL;
    if (lba + count > h->sector_count) return -EINVAL;

    spinlock_lock(&h->lock);

    const u8 *src = (const u8 *)buf;
    for (u32 s = 0; s < count; s++) {
        u64 sector = lba + s;
        u32 arg = (h->card_type == SD_TYPE_HIGH_CAPACITY) ? (u32)sector : (u32)(sector * 512);

        sdhci_write16(h, SDHCI_BLOCK_SIZE, 512);
        sdhci_write16(h, SDHCI_BLOCK_COUNT, 1);

        int rc = sdhci_send_cmd(h, MMC_WRITE_BLOCK, arg,
                                SDHCI_TRNS_BLK_CNT_EN,
                                SDHCI_CMD_RESP_48, true, NULL);
        if (rc != 0) {
            spinlock_unlock(&h->lock);
            return rc;
        }

        /* Wait for Buffer Write Ready */
        bool ready = false;
        for (u32 i = 0; i < 200000; i++) {
            if (sdhci_read16(h, SDHCI_INT_STATUS) & SDHCI_INT_BUF_WRITE_READY) {
                sdhci_write16(h, SDHCI_INT_STATUS, SDHCI_INT_BUF_WRITE_READY);
                ready = true;
                break;
            }
            __asm__ volatile("pause");
        }
        if (!ready) {
            spinlock_unlock(&h->lock);
            return -EIO;
        }

        /* Write 512 bytes (128 dwords) to buffer port */
        const u32 *p = (const u32 *)(src + s * 512);
        for (int i = 0; i < 128; i++) {
            sdhci_write32(h, SDHCI_BUFFER, p[i]);
        }

        /* Wait for transfer complete */
        for (u32 i = 0; i < 100000; i++) {
            if (sdhci_read16(h, SDHCI_INT_STATUS) & SDHCI_INT_XFER_COMPLETE) {
                sdhci_write16(h, SDHCI_INT_STATUS, SDHCI_INT_XFER_COMPLETE);
                break;
            }
            __asm__ volatile("pause");
        }
    }

    spinlock_unlock(&h->lock);
    return (s64)count * 512;
}

static s64 sdhci_flush(block_dev_t *bdev)
{
    (void)bdev;
    return 0;
}

static block_ops_t g_sdhci_bdev_ops = {
    .read_sectors  = sdhci_read_sectors,
    .write_sectors = sdhci_write_sectors,
    .flush         = sdhci_flush,
    .trim          = NULL,
};

/* ── PCI Driver Probe & Binding ───────────────────────────────────────────── */

static int sdhci_probe(dm_device_t *dm, const pci_device_id_t *id)
{
    (void)id;
    if (g_sdhci_host_count >= SDHCI_MAX_HOSTS) return -ENOMEM;

    pci_device_info_t *pci = to_pci_info(dm);
    if (!pci || !dm->hal) return -ENODEV;

    device_t *dev = dm->hal;
    pci_enable_bus_mastering(dev);

    phys_addr_t bar_phys = pci_get_bar(dev, 0);
    if (!bar_phys) return -ENODEV;

    sdhci_host_t *host = &g_sdhci_hosts[g_sdhci_host_count];
    memset(host, 0, sizeof(*host));
    host->mmio_base = (uintptr_t)PHYS_TO_VIRT(bar_phys);
    host->mmio_size = 0x1000;
    spinlock_init(&host->lock);

    pr_debug("[SDHCI] Host controller %u found at PCI %02x:%02x.%x, MMIO=0x%lx\n",
             g_sdhci_host_count, pci->bus, pci->slot, pci->func, (unsigned long)bar_phys);

    int rc = sdhci_card_init(host);
    if (rc == 0 && host->sector_count > 0) {
        memset(&host->bdev, 0, sizeof(host->bdev));
        host->bdev.sector_size  = host->sector_size;
        host->bdev.sector_count = host->sector_count;
        host->bdev.ops          = &g_sdhci_bdev_ops;
        host->bdev.driver_data  = host;

        scnprintf(host->bdev.name, sizeof(host->bdev.name), "mmcblk%u", g_sdhci_host_count);

        if (block_dev_register(&host->bdev) == 0) {
            u64 mib = host->capacity_bytes / (1024 * 1024);
            pr_debug("[SDHCI] Registered /dev/%s: %s card, %llu MiB (%llu sectors)\n",
                     host->bdev.name,
                     (host->card_type == SD_TYPE_HIGH_CAPACITY) ? "SDHC/SDXC" : "SDSC",
                     (unsigned long long)mib, (unsigned long long)host->sector_count);
        }
    }

    g_sdhci_host_count++;
    return 0;
}

static const pci_device_id_t sdhci_pci_ids[] = {
    /* Realtek PCIe Card Reader Controllers */
    { PCI_DEVICE(0x10EC, 0x5209) },
    { PCI_DEVICE(0x10EC, 0x5227) },
    { PCI_DEVICE(0x10EC, 0x5229) },
    { PCI_DEVICE(0x10EC, 0x5287) },
    { PCI_DEVICE(0x10EC, 0x5289) },

    /* Intel PCH SDHCI */
    { PCI_DEVICE(0x8086, 0x02C4) },
    { PCI_DEVICE(0x8086, 0x06C4) },
    { PCI_DEVICE(0x8086, 0x9D2D) },
    { PCI_DEVICE(0x8086, 0xA12D) },
    { PCI_DEVICE(0x8086, 0xA2A4) },
    { PCI_DEVICE(0x8086, 0xA32D) },

    /* O2Micro, Ricoh, TI, JMicron */
    { PCI_DEVICE(0x1217, 0x7120) },
    { PCI_DEVICE(0x1217, 0x8221) },
    { PCI_DEVICE(0x1180, 0x0822) },
    { PCI_DEVICE(0x1180, 0xE822) },
    { PCI_DEVICE(0x104C, 0x8034) },
    { PCI_DEVICE(0x197B, 0x2381) },
    { PCI_DEVICE(0x197B, 0x2391) },

    /* Match all standard PCI SD Host Controllers (Class 0x08, Subclass 0x05) */
    { PCI_DEVICE_CLASS(0x080500, 0xFFFF00) },
    { 0 }
};

static pci_driver_t sdhci_pci_driver = {
    .drv = {
        .name = "sdhci",
    },
    .id_table = sdhci_pci_ids,
    .probe    = sdhci_probe,
    .remove   = NULL,
};

void sdhci_init(void)
{
    pci_driver_register(&sdhci_pci_driver);
    pr_debug("[SDHCI] SD/MMC Host Controller Interface driver registered\n");
}
