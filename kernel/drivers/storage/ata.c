/**
 * ata.c — ATA PIO driver for AzamiOS
 *
 * Supports LBA28 and LBA48, both read and write, PIO polling.
 * Acts as fallback when AHCI is absent.
 */
#include "include/ata.h"
#include "../klibc/include/port.h"
#include "../klibc/include/stdio.h"
#include "../klibc/include/string.h"
#include <stdbool.h>

/* ── Channel state ───────────────────────────────────────────────────────── */

typedef struct {
    uint16_t    base;
    uint16_t    ctrl;
    uint8_t     drive_sel;
    bool        lba48;
    uint64_t    sector_count;
    const char *name;
} ata_channel_t;

static ata_channel_t g_chan;
static block_device_t ata_device;

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static inline void ata_delay400(const ata_channel_t *c) {
    inb(c->ctrl); inb(c->ctrl); inb(c->ctrl); inb(c->ctrl);
}

static int ata_poll(const ata_channel_t *c) {
    for (int i = 0; i < 100000; i++) {
        uint8_t s = inb((uint16_t)(c->base + 7));
        if (!(s & ATA_SR_BSY)) {
            if (s & (ATA_SR_ERR | ATA_SR_DF)) return -1;
            return 0;
        }
    }
    return -2;
}

static int ata_wait_drq(const ata_channel_t *c) {
    for (int i = 0; i < 100000; i++) {
        uint8_t s = inb((uint16_t)(c->base + 7));
        if (s & (ATA_SR_ERR | ATA_SR_DF)) return -1;
        if (s & ATA_SR_DRQ) return 0;
    }
    return -2;
}

/* ── Read (LBA28 + LBA48) ────────────────────────────────────────────────── */

int ata_read_sectors(uint32_t lba, uint8_t count, uint16_t *buf) {
    ata_channel_t *c = &g_chan;
    if (g_chan.lba48 && ((uint64_t)lba + count > 0x0FFFFFFF)) {
        /* LBA48 path */
        outb((uint16_t)(c->base + 6), (uint8_t)(c->drive_sel | 0x40));
        outb((uint16_t)(c->base + 1), 0x00);
        outb((uint16_t)(c->base + 2), 0x00);                    /* count hi */
        outb((uint16_t)(c->base + 3), (uint8_t)(lba >> 24));
        outb((uint16_t)(c->base + 4), (uint8_t)(((uint64_t)lba) >> 32));
        outb((uint16_t)(c->base + 5), (uint8_t)(((uint64_t)lba) >> 40));
        outb((uint16_t)(c->base + 1), 0x00);
        outb((uint16_t)(c->base + 2), count);
        outb((uint16_t)(c->base + 3), (uint8_t)(lba));
        outb((uint16_t)(c->base + 4), (uint8_t)(lba >>  8));
        outb((uint16_t)(c->base + 5), (uint8_t)(lba >> 16));
        outb((uint16_t)(c->base + 7), ATA_CMD_READ_PIO_EXT);
    } else {
        /* LBA28 path */
        outb((uint16_t)(c->base + 6),
             (uint8_t)(c->drive_sel | 0x40 | ((lba >> 24) & 0x0F)));
        outb((uint16_t)(c->base + 1), 0x00);
        outb((uint16_t)(c->base + 2), count);
        outb((uint16_t)(c->base + 3), (uint8_t)(lba));
        outb((uint16_t)(c->base + 4), (uint8_t)(lba >>  8));
        outb((uint16_t)(c->base + 5), (uint8_t)(lba >> 16));
        outb((uint16_t)(c->base + 7), ATA_CMD_READ_PIO);
    }

    for (uint8_t s = 0; s < count; s++) {
        ata_delay400(c);
        if (ata_poll(c) != 0 || ata_wait_drq(c) != 0) {
            kprintf("ata: read error sector %u\n", lba + s);
            return -1;
        }
        uint16_t *dest = buf + s * 256;
        for (int i = 0; i < 256; i++) dest[i] = inw(c->base);
    }
    return 0;
}

/* ── Write (LBA28 + LBA48) ───────────────────────────────────────────────── */

int ata_write_sectors(uint32_t lba, uint8_t count, const uint16_t *buf) {
    ata_channel_t *c = &g_chan;
    if (g_chan.lba48 && ((uint64_t)lba + count > 0x0FFFFFFF)) {
        outb((uint16_t)(c->base + 6), (uint8_t)(c->drive_sel | 0x40));
        outb((uint16_t)(c->base + 1), 0x00);
        outb((uint16_t)(c->base + 2), 0x00);
        outb((uint16_t)(c->base + 3), (uint8_t)(lba >> 24));
        outb((uint16_t)(c->base + 4), (uint8_t)(((uint64_t)lba) >> 32));
        outb((uint16_t)(c->base + 5), (uint8_t)(((uint64_t)lba) >> 40));
        outb((uint16_t)(c->base + 1), 0x00);
        outb((uint16_t)(c->base + 2), count);
        outb((uint16_t)(c->base + 3), (uint8_t)(lba));
        outb((uint16_t)(c->base + 4), (uint8_t)(lba >>  8));
        outb((uint16_t)(c->base + 5), (uint8_t)(lba >> 16));
        outb((uint16_t)(c->base + 7), ATA_CMD_WRITE_PIO_EXT);
    } else {
        outb((uint16_t)(c->base + 6),
             (uint8_t)(c->drive_sel | 0x40 | ((lba >> 24) & 0x0F)));
        outb((uint16_t)(c->base + 1), 0x00);
        outb((uint16_t)(c->base + 2), count);
        outb((uint16_t)(c->base + 3), (uint8_t)(lba));
        outb((uint16_t)(c->base + 4), (uint8_t)(lba >>  8));
        outb((uint16_t)(c->base + 5), (uint8_t)(lba >> 16));
        outb((uint16_t)(c->base + 7), ATA_CMD_WRITE_PIO);
    }

    for (uint8_t s = 0; s < count; s++) {
        ata_delay400(c);
        if (ata_poll(c) != 0 || ata_wait_drq(c) != 0) {
            kprintf("ata: write error sector %u\n", lba + s);
            return -1;
        }
        const uint16_t *src = buf + s * 256;
        for (int i = 0; i < 256; i++) outw(c->base, src[i]);
        /* Cache flush */
        outb((uint16_t)(c->base + 7), 0xE7); /* FLUSH CACHE */
        ata_poll(c);
    }
    return 0;
}

/* ── block_device_t callbacks ────────────────────────────────────────────── */

static uint32_t ata_block_read(block_device_t *dev,
                                uint32_t lba, uint32_t count, void *buffer) {
    (void)dev;
    uint8_t *buf8 = (uint8_t *)buffer;
    for (uint32_t s = 0; s < count; s++) {
        uint16_t tmp[256];
        if (ata_read_sectors(lba + s, 1, tmp) != 0) return s * 512;
        memcpy(buf8 + s * 512, tmp, 512);
    }
    return count * 512;
}

static uint32_t ata_block_write(block_device_t *dev,
                                 uint32_t lba, uint32_t count, void *buffer) {
    (void)dev;
    const uint8_t *buf8 = (const uint8_t *)buffer;
    for (uint32_t s = 0; s < count; s++) {
        uint16_t tmp[256];
        memcpy(tmp, buf8 + s * 512, 512);
        if (ata_write_sectors(lba + s, 1, tmp) != 0) return s * 512;
    }
    return count * 512;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

int ata_init(void) {
    struct { uint16_t base; uint16_t ctrl; uint8_t drv; const char *n; } chans[4] = {
        { ATA_PRI_DATA, ATA_PRI_CTRL, ATA_DRIVE_MASTER, "IDE0-M" },
        { ATA_PRI_DATA, ATA_PRI_CTRL, ATA_DRIVE_SLAVE,  "IDE0-S" },
        { ATA_SEC_DATA, ATA_SEC_CTRL, ATA_DRIVE_MASTER, "IDE1-M" },
        { ATA_SEC_DATA, ATA_SEC_CTRL, ATA_DRIVE_SLAVE,  "IDE1-S" },
    };

    for (int ch = 0; ch < 4; ch++) {
        ata_channel_t c;
        c.base      = chans[ch].base;
        c.ctrl      = chans[ch].ctrl;
        c.drive_sel = chans[ch].drv;
        c.name      = chans[ch].n;
        c.lba48     = false;
        c.sector_count = 0;

        outb((uint16_t)(c.base + 6), c.drive_sel);
        ata_delay400(&c);

        uint8_t status = inb((uint16_t)(c.base + 7));
        if (status == 0x00 || status == 0xFF) continue;

        /* IDENTIFY */
        outb((uint16_t)(c.base + 2), 0);
        outb((uint16_t)(c.base + 3), 0);
        outb((uint16_t)(c.base + 4), 0);
        outb((uint16_t)(c.base + 5), 0);
        outb((uint16_t)(c.base + 7), ATA_CMD_IDENTIFY);
        ata_delay400(&c);

        status = inb((uint16_t)(c.base + 7));
        if (!status) continue;

        for (int i = 0; i < 100000; i++) {
            status = inb((uint16_t)(c.base + 7));
            if (!(status & ATA_SR_BSY)) break;
        }
        uint8_t mid = inb((uint16_t)(c.base + 4));
        uint8_t hi  = inb((uint16_t)(c.base + 5));
        if (mid || hi) continue; /* ATAPI or other non-ATA */

        if (ata_wait_drq(&c) != 0) continue;

        uint16_t id[256];
        for (int i = 0; i < 256; i++) id[i] = inw(c.base);

        char model[41];
        for (int i = 0; i < 20; i++) {
            model[i * 2]     = (char)(id[27 + i] >> 8);
            model[i * 2 + 1] = (char)(id[27 + i] & 0xFF);
        }
        model[40] = '\0';

        /* LBA48 support: bit 10 of word 83 */
        c.lba48 = (id[83] & (1u << 10)) != 0;
        uint64_t lba28  = ((uint32_t)id[61] << 16) | id[60];
        uint64_t lba48  = ((uint64_t)id[103] << 48) | ((uint64_t)id[102] << 32) |
                          ((uint64_t)id[101] << 16) |  (uint64_t)id[100];
        c.sector_count  = (c.lba48 && lba48) ? lba48 : lba28;

        kprintf("ata: %s: %s LBA%s %llu sectors (%llu MiB)\n",
                c.name, model,
                c.lba48 ? "48" : "28",
                (unsigned long long)c.sector_count,
                (unsigned long long)(c.sector_count / 2048));

        g_chan = c;

        memset(&ata_device, 0, sizeof(ata_device));
        memcpy(ata_device.name, "hda", 4);
        ata_device.block_size = 512;
        ata_device.read  = ata_block_read;
        ata_device.write = ata_block_write;
        vfs_register_device(&ata_device);
        return 0;
    }

    kprintf("ata: no ATA disk found\n");
    return -1;
}

block_device_t *ata_get_device(void) { return &ata_device; }
