/* ============================================================================
 * AzamiOS — Floppy Disk Controller (FDC) Driver Header
 * File: drivers/block/fdc.h
 * ============================================================================ */
#pragma once

#include "../../include/azami/types.h"

#define FDC_SECTOR_SIZE 512
#define FDC_SECTORS_1440 2880

/** fdc_init() — Probe and initialize Floppy Disk Controller. */
int fdc_init(void);

/** fdc_read_sectors() — Read sectors from floppy disk. */
s64 fdc_read_sectors(u64 lba, u32 count, void *buf);

/** fdc_write_sectors() — Write sectors to floppy disk. */
s64 fdc_write_sectors(u64 lba, u32 count, const void *buf);
