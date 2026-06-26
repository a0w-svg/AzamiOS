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

/* ─── internal helpers ───────────────────────────────────────────────────── */

/** Delay ~400 ns by reading the alternate-status register four times. */
static inline void ata_delay400(void) {
    inb(ATA_PRI_CTRL);
    inb(ATA_PRI_CTRL);
    inb(ATA_PRI_CTRL);
    inb(ATA_PRI_CTRL);
}

/**
 * ata_poll – wait until BSY clears, then check for errors.
 * @return 0 on success, -1 on error or device fault, -2 on timeout.
 */
static int ata_poll(void) {
    /* Spin until BSY clears (up to ~30 000 iterations). */
    for (int i = 0; i < 30000; i++) {
        uint8_t status = inb(ATA_PRI_STATUS);
        if (!(status & ATA_SR_BSY)) {
            if (status & ATA_SR_ERR) { return -1; }
            if (status & ATA_SR_DF)  { return -1; }
            return 0;
        }
    }
    return -2; /* timeout */
}

/**
 * ata_wait_drq – wait until DRQ (data-request) is set.
 * @return 0 on success, -1 on error/fault, -2 on timeout.
 */
static int ata_wait_drq(void) {
    for (int i = 0; i < 30000; i++) {
        uint8_t status = inb(ATA_PRI_STATUS);
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
    /* Soft-reset the primary channel */
    outb(ATA_PRI_CTRL, 0x04); /* SRST bit */
    ata_delay400();
    outb(ATA_PRI_CTRL, 0x00); /* clear SRST */
    ata_delay400();

    /* Select primary master */
    outb(ATA_PRI_DRIVE, ATA_DRIVE_MASTER);
    ata_delay400();

    /* Wait for BSY to clear after reset */
    for (int i = 0; i < 30000; i++) {
        uint8_t s = inb(ATA_PRI_STATUS);
        if (!(s & ATA_SR_BSY)) break;
    }

    /* Check if a drive is attached: status should not be 0x00 or 0xFF */
    uint8_t status = inb(ATA_PRI_STATUS);
    if (status == 0x00 || status == 0xFF) {
        kprintf("ata: no drive detected (status=0x%x)\n", status);
        return -1;
    }

    /* Issue IDENTIFY */
    outb(ATA_PRI_SECCOUNT, 0);
    outb(ATA_PRI_LBA_LO,   0);
    outb(ATA_PRI_LBA_MID,  0);
    outb(ATA_PRI_LBA_HI,   0);
    outb(ATA_PRI_CMD, ATA_CMD_IDENTIFY);
    ata_delay400();

    status = inb(ATA_PRI_STATUS);
    if (status == 0) {
        kprintf("ata: drive does not exist\n");
        return -1;
    }

    /* Wait for BSY to clear */
    for (int i = 0; i < 30000; i++) {
        status = inb(ATA_PRI_STATUS);
        if (!(status & ATA_SR_BSY)) break;
    }

    /* If LBA_MID or LBA_HI are non-zero, it's not an ATA drive */
    if (inb(ATA_PRI_LBA_MID) || inb(ATA_PRI_LBA_HI)) {
        kprintf("ata: not an ATA device\n");
        return -1;
    }

    /* Wait for DRQ or ERR */
    if (ata_wait_drq() != 0) {
        kprintf("ata: IDENTIFY failed\n");
        return -1;
    }

    /* Read and discard the 256-word IDENTIFY buffer */
    uint16_t identify[256];
    for (int i = 0; i < 256; i++) {
        identify[i] = inw(ATA_PRI_DATA);
    }

    /* Extract drive model string (words 27–46, big-endian byte pairs) */
    char model[41];
    for (int i = 0; i < 20; i++) {
        model[i * 2]     = (char)(identify[27 + i] >> 8);
        model[i * 2 + 1] = (char)(identify[27 + i] & 0xFF);
    }
    model[40] = '\0';
    kprintf("ata: drive detected: %s\n", model);

    /* Fill in the block_device_t */
    memset(&ata_device, 0, sizeof(ata_device));
    memcpy(ata_device.name, "ata0", 5);
    ata_device.block_size = 512;
    ata_device.read  = ata_block_read;
    ata_device.write = 0; /* read-only for now */

    return 0;
}

int ata_read_sectors(uint32_t lba, uint8_t count, uint16_t *buf) {
    /* Select drive and set LBA bits 24-27 */
    outb(ATA_PRI_DRIVE,   (uint8_t)(ATA_DRIVE_MASTER | ((lba >> 24) & 0x0F)));
    outb(ATA_PRI_ERROR,   0x00);
    outb(ATA_PRI_SECCOUNT, count);
    outb(ATA_PRI_LBA_LO,  (uint8_t)(lba & 0xFF));
    outb(ATA_PRI_LBA_MID, (uint8_t)((lba >> 8)  & 0xFF));
    outb(ATA_PRI_LBA_HI,  (uint8_t)((lba >> 16) & 0xFF));
    outb(ATA_PRI_CMD,     ATA_CMD_READ_PIO);

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

        /* Read 256 words (512 bytes) from the data port */
        uint16_t *dest = buf + s * 256;
        for (int i = 0; i < 256; i++) {
            dest[i] = inw(ATA_PRI_DATA);
        }
    }
    return 0;
}

block_device_t *ata_get_device(void) {
    return &ata_device;
}
