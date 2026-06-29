/**
 * ata.c  –  ATA PIO polling driver for AzamiOS
 *
 * Supports the primary master drive only (I/O base 0x1F0).
 * Uses 28-bit LBA, PIO polling (no DMA, no IRQs needed).
 *
 * Reference: ATA/ATAPI-6 specification, OSDev wiki "ATA PIO Mode".
 */

#include "include/ata.h"
#include "../klibc/include/port.h"
#include "../klibc/include/stdio.h"
#include "../klibc/include/string.h"

/* ─── Active channel tracking ────────────────────────────────────────────── */
static uint16_t g_ata_base  = ATA_PRI_DATA;
static uint16_t g_ata_ctrl  = ATA_PRI_CTRL;
static uint8_t  g_ata_drive = ATA_DRIVE_MASTER;

/* ─── internal helpers ───────────────────────────────────────────────────── */

/** Delay ~400 ns by reading the alternate-status register four times. */
static inline void ata_delay400(void) {
    inb(g_ata_ctrl);
    inb(g_ata_ctrl);
    inb(g_ata_ctrl);
    inb(g_ata_ctrl);
}

/**
 * ata_poll – wait until BSY clears, then check for errors.
 * @return 0 on success, -1 on error or device fault, -2 on timeout.
 */
static int ata_poll(void) {
    for (int i = 0; i < 30000; i++) {
        uint8_t status = inb(g_ata_base + 7);
        if (!(status & ATA_SR_BSY)) {
            if (status & ATA_SR_ERR) { return -1; }
            if (status & ATA_SR_DF)  { return -1; }
            return 0;
        }
    }
    return -2;
}

/**
 * ata_wait_drq – wait until DRQ (data-request) is set.
 * @return 0 on success, -1 on error/fault, -2 on timeout.
 */
static int ata_wait_drq(void) {
    for (int i = 0; i < 30000; i++) {
        uint8_t status = inb(g_ata_base + 7);
        if (status & ATA_SR_ERR) { return -1; }
        if (status & ATA_SR_DF)  { return -1; }
        if (status & ATA_SR_DRQ) { return 0; }
    }
    return -2;
}


/* ─── VFS block-device glue ──────────────────────────────────────────────── */

/**
 * ata_block_read – block_device_t read callback.
 * @param lba   raw sector number (LBA28).
 * @param count sector count to read.
 */
static uint32_t ata_block_read(block_device_t *dev,
                                uint32_t lba,
                                uint32_t count,
                                void *buffer) {
    (void)dev;
    uint8_t *buf = (uint8_t *)buffer;

    for (uint32_t s = 0; s < count; s++) {
        uint16_t tmp[256];
        if (ata_read_sectors(lba + s, 1, tmp) != 0) {
            return s * 512;
        }
        memcpy(buf + s * 512, tmp, 512);
    }
    return count * 512;
}

static block_device_t ata_device; /* statically allocated */

/* ─── Public API ─────────────────────────────────────────────────────────── */

int ata_init(void) {
    struct {
        uint16_t base;
        uint16_t ctrl;
        uint8_t  drive;
        const char *name;
    } channels[4] = {
        { ATA_PRI_DATA, ATA_PRI_CTRL, ATA_DRIVE_MASTER, "IDE0 Master" },
        { ATA_PRI_DATA, ATA_PRI_CTRL, ATA_DRIVE_SLAVE,  "IDE0 Slave"  },
        { ATA_SEC_DATA, ATA_SEC_CTRL, ATA_DRIVE_MASTER, "IDE1 Master" },
        { ATA_SEC_DATA, ATA_SEC_CTRL, ATA_DRIVE_SLAVE,  "IDE1 Slave"  }
    };

    for (int ch = 0; ch < 4; ch++) {
        g_ata_base  = channels[ch].base;
        g_ata_ctrl  = channels[ch].ctrl;
        g_ata_drive = channels[ch].drive;

        /* Select drive */
        outb(g_ata_base + 6, g_ata_drive);
        ata_delay400();

        /* Check if floating bus */
        uint8_t status = inb(g_ata_base + 7);
        if (status == 0x00 || status == 0xFF) continue;

        /* Issue IDENTIFY */
        outb(g_ata_base + 2, 0);
        outb(g_ata_base + 3, 0);
        outb(g_ata_base + 4, 0);
        outb(g_ata_base + 5, 0);
        outb(g_ata_base + 7, ATA_CMD_IDENTIFY);
        ata_delay400();

        status = inb(g_ata_base + 7);
        if (status == 0) continue;

        for (int i = 0; i < 30000; i++) {
            status = inb(g_ata_base + 7);
            if (!(status & ATA_SR_BSY)) break;
        }

        uint8_t mid = inb(g_ata_base + 4);
        uint8_t hi  = inb(g_ata_base + 5);
        if (mid || hi) {
            if ((mid == 0x14 && hi == 0xEB) || (mid == 0x69 && hi == 0x96)) {
                kprintf("ata: ATAPI CD-ROM detected on %s\n", channels[ch].name);
            }
            continue;
        }

        if (ata_wait_drq() != 0) continue;

        uint16_t identify[256];
        for (int i = 0; i < 256; i++) identify[i] = inw(g_ata_base);

        char model[41];
        for (int i = 0; i < 20; i++) {
            model[i * 2]     = (char)(identify[27 + i] >> 8);
            model[i * 2 + 1] = (char)(identify[27 + i] & 0xFF);
        }
        model[40] = '\0';
        kprintf("ata: hard disk detected on %s: %s\n", channels[ch].name, model);

        memset(&ata_device, 0, sizeof(ata_device));
        memcpy(ata_device.name, "hda", 4);
        ata_device.block_size = 512;
        ata_device.read  = ata_block_read;
        ata_device.write = 0;
        vfs_register_device(&ata_device);
        return 0;
    }

    kprintf("ata: no ATA hard disk detected on any IDE channel\n");
    return -1;
}

int ata_read_sectors(uint32_t lba, uint8_t count, uint16_t *buf) {
    outb(g_ata_base + 6, (uint8_t)(g_ata_drive | ((lba >> 24) & 0x0F)));
    outb(g_ata_base + 1, 0x00);
    outb(g_ata_base + 2, count);
    outb(g_ata_base + 3, (uint8_t)(lba & 0xFF));
    outb(g_ata_base + 4, (uint8_t)((lba >> 8)  & 0xFF));
    outb(g_ata_base + 5, (uint8_t)((lba >> 16) & 0xFF));
    outb(g_ata_base + 7, ATA_CMD_READ_PIO);

    for (uint8_t s = 0; s < count; s++) {
        ata_delay400();
        if (ata_poll() != 0) {
            kprintf("ata: poll error on sector %d\n", lba + s);
            return -1;
        }
        if (ata_wait_drq() != 0) {
            kprintf("ata: DRQ timeout on sector %d\n", lba + s);
            return -1;
        }
        uint16_t *dest = buf + s * 256;
        for (int i = 0; i < 256; i++) {
            dest[i] = inw(g_ata_base);
        }
    }
    return 0;
}


block_device_t *ata_get_device(void) {
    return &ata_device;
}
