#ifndef ATA_H
#define ATA_H

#include <stdint.h>
#include "../../filesystem/include/vfs.h"

/* ─── Primary ATA channel I/O ports ──────────────────────────────────────── */
#define ATA_PRI_DATA        0x1F0   /* 16-bit data register          */
#define ATA_PRI_ERROR       0x1F1   /* error / features              */
#define ATA_PRI_SECCOUNT    0x1F2   /* sector count                  */
#define ATA_PRI_LBA_LO      0x1F3   /* LBA bits  0– 7               */
#define ATA_PRI_LBA_MID     0x1F4   /* LBA bits  8–15               */
#define ATA_PRI_LBA_HI      0x1F5   /* LBA bits 16–23               */
#define ATA_PRI_DRIVE       0x1F6   /* drive select / LBA bits 24–27 */
#define ATA_PRI_STATUS      0x1F7   /* status  (read)                */
#define ATA_PRI_CMD         0x1F7   /* command (write)               */
#define ATA_PRI_CTRL        0x3F6   /* alternate status / device ctrl */

/* ─── Status register bits ───────────────────────────────────────────────── */
#define ATA_SR_BSY          0x80    /* controller busy               */
#define ATA_SR_DRDY         0x40    /* drive ready                   */
#define ATA_SR_DF           0x20    /* device fault                  */
#define ATA_SR_DRQ          0x08    /* data request                  */
#define ATA_SR_ERR          0x01    /* error                         */

/* ─── Commands ───────────────────────────────────────────────────────────── */
#define ATA_CMD_READ_PIO    0x20    /* read sectors (LBA28, PIO)     */
#define ATA_CMD_IDENTIFY    0xEC    /* identify device               */

/* ─── Drive select nibble (LBA mode) ─────────────────────────────────────── */
#define ATA_DRIVE_MASTER    0xE0    /* primary master, LBA           */

/* ─── Public API ──────────────────────────────────────────────────────────── */

/**
 * ata_init – soft-reset the primary channel and IDENTIFY the master drive.
 * @return  0 on success, -1 if no drive present or IDENTIFY failed.
 */
int ata_init(void);

/**
 * ata_read_sectors – read @count 512-byte sectors beginning at LBA @lba
 *                    into @buf (PIO polling, primary master).
 * @return  0 on success, -1 on timeout or error.
 */
int ata_read_sectors(uint32_t lba, uint8_t count, uint16_t *buf);

/**
 * ata_get_device – return a pointer to the statically allocated block_device_t
 *                  that wraps the primary master.  Valid after ata_init() == 0.
 */
block_device_t *ata_get_device(void);

#endif /* ATA_H */
