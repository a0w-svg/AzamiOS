/* ============================================================================
 * AzamiOS — ATA / IDE PIO Block Device Driver (ata.c)
 * File: drivers/block/ata.c
 * ============================================================================ */

#define DEBUG 1
#include <azami/debug.h>
#include "ata.h"
#include "../../include/azami/defs.h"
#include "../../kernel/mm/kmalloc.h"
#include "../../kernel/lib/string.h"
#include "../../arch/x86_64/cpu/spinlock.h"

typedef struct ata_drive {
    u16 io_base;
    u16 ctrl_base;
    u8  slave;          /* 0 = Master, 1 = Slave */
    u8  is_atapi;
    u8  lba48;
    u64 sector_count;
    char model[41];
    block_dev_t bdev;
} ata_drive_t;

static spinlock_t g_ata_lock = SPINLOCK_INIT;
static ata_drive_t g_ata_drives[4];
static u32 g_ata_count = 0;

static void ata_delay(u16 ctrl)
{
    /* Read status 4 times (approx 400ns delay) */
    inb(ctrl + 2);
    inb(ctrl + 2);
    inb(ctrl + 2);
    inb(ctrl + 2);
}

static int ata_poll(u16 io_base, u8 check_drq)
{
    for (int i = 0; i < 100000; i++) {
        u8 status = inb(io_base + ATA_REG_STATUS);
        if (status == 0xFF) return -1; /* Floating bus */
        if (status & ATA_SR_ERR) return -1;
        if (status & ATA_SR_DF) return -1;

        if (!(status & ATA_SR_BSY)) {
            if (!check_drq || (status & ATA_SR_DRQ)) {
                return 0;
            }
        }
    }
    return -1; /* Timeout */
}

static s64 ata_read_sectors(block_dev_t *dev, u64 lba, u32 count, void *buffer)
{
    ata_drive_t *drive = (ata_drive_t *)dev->driver_data;
    if (!drive || count == 0) return -1;

    spinlock_lock(&g_ata_lock);

    u16 *buf16 = (u16 *)buffer;
    u16 io = drive->io_base;
    u8 slave = drive->slave;

    for (u32 s = 0; s < count; s++) {
        u64 cur_lba = lba + s;

        if (drive->lba48 && (cur_lba >= 0x0FFFFFFF || (cur_lba + count) > 0x0FFFFFFF)) {
            outb(io + ATA_REG_HDDEVSEL, 0x40 | (slave << 4));
            outb(io + ATA_REG_SECCOUNT0, 0);
            outb(io + ATA_REG_LBA0, (u8)(cur_lba >> 24));
            outb(io + ATA_REG_LBA1, (u8)(cur_lba >> 32));
            outb(io + ATA_REG_LBA2, (u8)(cur_lba >> 40));
            outb(io + ATA_REG_SECCOUNT0, 1);
            outb(io + ATA_REG_LBA0, (u8)(cur_lba));
            outb(io + ATA_REG_LBA1, (u8)(cur_lba >> 8));
            outb(io + ATA_REG_LBA2, (u8)(cur_lba >> 16));
            outb(io + ATA_REG_COMMAND, ATA_CMD_READ_PIO_EXT);
        } else {
            outb(io + ATA_REG_HDDEVSEL, 0xE0 | (slave << 4) | ((cur_lba >> 24) & 0x0F));
            outb(io + ATA_REG_FEATURES, 0x00);
            outb(io + ATA_REG_SECCOUNT0, 1);
            outb(io + ATA_REG_LBA0, (u8)cur_lba);
            outb(io + ATA_REG_LBA1, (u8)(cur_lba >> 8));
            outb(io + ATA_REG_LBA2, (u8)(cur_lba >> 16));
            outb(io + ATA_REG_COMMAND, ATA_CMD_READ_PIO);
        }

        ata_delay(drive->ctrl_base);

        if (ata_poll(io, 1) != 0) {
            spinlock_unlock(&g_ata_lock);
            return -1;
        }

        for (int i = 0; i < 256; i++) {
            *buf16++ = inw(io + ATA_REG_DATA);
        }
    }

    spinlock_unlock(&g_ata_lock);
    return count * 512;
}

static s64 ata_write_sectors(block_dev_t *dev, u64 lba, u32 count, const void *buffer)
{
    ata_drive_t *drive = (ata_drive_t *)dev->driver_data;
    if (!drive || count == 0) return -1;

    spinlock_lock(&g_ata_lock);

    const u16 *buf16 = (const u16 *)buffer;
    u16 io = drive->io_base;
    u8 slave = drive->slave;

    for (u32 s = 0; s < count; s++) {
        u64 cur_lba = lba + s;

        if (drive->lba48 && (cur_lba >= 0x0FFFFFFF || (cur_lba + count) > 0x0FFFFFFF)) {
            outb(io + ATA_REG_HDDEVSEL, 0x40 | (slave << 4));
            outb(io + ATA_REG_SECCOUNT0, 0);
            outb(io + ATA_REG_LBA0, (u8)(cur_lba >> 24));
            outb(io + ATA_REG_LBA1, (u8)(cur_lba >> 32));
            outb(io + ATA_REG_LBA2, (u8)(cur_lba >> 40));
            outb(io + ATA_REG_SECCOUNT0, 1);
            outb(io + ATA_REG_LBA0, (u8)(cur_lba));
            outb(io + ATA_REG_LBA1, (u8)(cur_lba >> 8));
            outb(io + ATA_REG_LBA2, (u8)(cur_lba >> 16));
            outb(io + ATA_REG_COMMAND, ATA_CMD_WRITE_PIO_EXT);
        } else {
            outb(io + ATA_REG_HDDEVSEL, 0xE0 | (slave << 4) | ((cur_lba >> 24) & 0x0F));
            outb(io + ATA_REG_FEATURES, 0x00);
            outb(io + ATA_REG_SECCOUNT0, 1);
            outb(io + ATA_REG_LBA0, (u8)cur_lba);
            outb(io + ATA_REG_LBA1, (u8)(cur_lba >> 8));
            outb(io + ATA_REG_LBA2, (u8)(cur_lba >> 16));
            outb(io + ATA_REG_COMMAND, ATA_CMD_WRITE_PIO);
        }

        ata_delay(drive->ctrl_base);

        if (ata_poll(io, 1) != 0) {
            spinlock_unlock(&g_ata_lock);
            return -1;
        }

        for (int i = 0; i < 256; i++) {
            outw(io + ATA_REG_DATA, *buf16++);
        }

        /* Flush cache */
        outb(io + ATA_REG_COMMAND, drive->lba48 ? ATA_CMD_FLUSH_EXT : ATA_CMD_FLUSH);
        ata_poll(io, 0);
    }

    spinlock_unlock(&g_ata_lock);
    return count * 512;
}

static block_ops_t g_ata_ops = {
    .read_sectors = ata_read_sectors,
    .write_sectors = ata_write_sectors,
};

static void ata_probe_drive(u16 io, u16 ctrl, u8 slave, const char *name)
{
    outb(io + ATA_REG_HDDEVSEL, 0xA0 | (slave << 4));
    ata_delay(ctrl);

    outb(io + ATA_REG_SECCOUNT0, 0);
    outb(io + ATA_REG_LBA0, 0);
    outb(io + ATA_REG_LBA1, 0);
    outb(io + ATA_REG_LBA2, 0);
    outb(io + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);
    ata_delay(ctrl);

    u8 status = inb(io + ATA_REG_STATUS);
    if (status == 0 || status == 0xFF) return; /* No drive */

    u8 cl = inb(io + ATA_REG_LBA1);
    u8 ch = inb(io + ATA_REG_LBA2);

    u8 is_atapi = 0;
    if (cl == 0x14 && ch == 0xEB) {
        is_atapi = 1;
        /* ATAPI CD-ROM */
        outb(io + ATA_REG_COMMAND, ATA_CMD_IDENTIFY_PACKET);
        ata_delay(ctrl);
    } else if (cl == 0x69 && ch == 0x96) {
        is_atapi = 1;
    } else if (cl != 0 || ch != 0) {
        return; /* Unknown device */
    }

    if (ata_poll(io, 1) != 0) return;

    u16 ident[256];
    for (int i = 0; i < 256; i++) {
        ident[i] = inw(io + ATA_REG_DATA);
    }

    ata_drive_t *d = &g_ata_drives[g_ata_count];
    d->io_base = io;
    d->ctrl_base = ctrl;
    d->slave = slave;
    d->is_atapi = is_atapi;

    /* Extract model name (words 27-46, byte-swapped) */
    for (int i = 0; i < 20; i++) {
        d->model[i * 2] = (char)(ident[27 + i] >> 8);
        d->model[i * 2 + 1] = (char)(ident[27 + i] & 0xFF);
    }
    d->model[40] = '\0';
    for (int i = 39; i >= 0 && d->model[i] == ' '; i--) {
        d->model[i] = '\0';
    }

    /* Sector count */
    if (ident[83] & (1 << 10)) {
        d->lba48 = 1;
        d->sector_count = ((u64)ident[103] << 48) | ((u64)ident[102] << 32) |
                          ((u64)ident[101] << 16) | ((u64)ident[100]);
    } else {
        d->lba48 = 0;
        d->sector_count = ((u32)ident[61] << 16) | ident[60];
    }

    strncpy(d->bdev.name, name, 31);
    d->bdev.name[31] = '\0';
    d->bdev.sector_size = 512;
    d->bdev.sector_count = d->sector_count;
    d->bdev.ops = &g_ata_ops;
    d->bdev.driver_data = d;

    block_dev_register(&d->bdev);
    g_ata_count++;

    pr_debug("[ATA] Found %s '%s' (%llu sectors, %llu MB, LBA%s)\n",
             name, d->model, (unsigned long long)d->sector_count,
             (unsigned long long)(d->sector_count * 512 / (1024 * 1024)),
             d->lba48 ? "48" : "28");
}

int ata_init(void)
{
    pr_debug("[ATA] Probing IDE/ATA primary and secondary channels...\n");
    ata_probe_drive(ATA_PRIMARY_IO, ATA_PRIMARY_CTRL, 0, "hda");
    ata_probe_drive(ATA_PRIMARY_IO, ATA_PRIMARY_CTRL, 1, "hdb");
    ata_probe_drive(ATA_SECONDARY_IO, ATA_SECONDARY_CTRL, 0, "hdc");
    ata_probe_drive(ATA_SECONDARY_IO, ATA_SECONDARY_CTRL, 1, "hdd");
    return (g_ata_count > 0) ? 0 : -1;
}
