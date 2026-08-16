/* ============================================================================
 * AzamiOS — ATA / IDE PIO Block Device Driver (ata.h)
 * File: drivers/block/ata.h
 * ============================================================================ */
#pragma once

#include "../../include/azami/types.h"
#include "block.h"

/* ATA I/O Ports */
#define ATA_PRIMARY_IO      0x1F0
#define ATA_PRIMARY_CTRL    0x3F6
#define ATA_SECONDARY_IO    0x170
#define ATA_SECONDARY_CTRL  0x376

/* ATA Registers (Offsets from IO Base) */
#define ATA_REG_DATA        0x00
#define ATA_REG_ERROR       0x01
#define ATA_REG_FEATURES    0x01
#define ATA_REG_SECCOUNT0   0x02
#define ATA_REG_LBA0        0x03
#define ATA_REG_LBA1        0x04
#define ATA_REG_LBA2        0x05
#define ATA_REG_HDDEVSEL    0x06
#define ATA_REG_COMMAND     0x07
#define ATA_REG_STATUS      0x07

/* Status bits */
#define ATA_SR_BSY          0x80    /* Busy */
#define ATA_SR_DRDY         0x40    /* Drive ready */
#define ATA_SR_DF           0x20    /* Drive write fault */
#define ATA_SR_DSC          0x10    /* Drive seek complete */
#define ATA_SR_DRQ          0x08    /* Data request ready */
#define ATA_SR_CORR         0x04    /* Corrected data */
#define ATA_SR_IDX          0x02    /* Index */
#define ATA_SR_ERR          0x01    /* Error */

/* Commands */
#define ATA_CMD_READ_PIO          0x20
#define ATA_CMD_READ_PIO_EXT      0x24
#define ATA_CMD_WRITE_PIO         0x30
#define ATA_CMD_WRITE_PIO_EXT     0x34
#define ATA_CMD_IDENTIFY          0xEC
#define ATA_CMD_IDENTIFY_PACKET   0xA1
#define ATA_CMD_FLUSH             0xE7
#define ATA_CMD_FLUSH_EXT         0xEA

int ata_init(void);
