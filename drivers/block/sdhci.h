/* ============================================================================
 * AzamiOS — SD / MMC Host Controller Interface (SDHCI) Header
 * File: drivers/block/sdhci.h
 *
 * Implements SDHCI Standard Spec v1.0 - v4.0 for PCI SD/MMC host controllers:
 *   - Realtek RTS5209, RTS5227, RTS5229, RTS5287, RTS5289 (PCI 10ec:52xx)
 *   - Intel Sunrise Point, Cannon Point, Tiger Lake, Alder Lake SDHCI
 *   - O2Micro, Ricoh, TI, JMicron PCI SD card controllers
 *   - QEMU / VirtualBox / real hardware PCI Class 0x08 Subclass 0x05
 * ============================================================================ */
#pragma once

#include "../../include/azami/types.h"
#include "../../include/azami/defs.h"
#include "block.h"
#include "../../arch/x86_64/cpu/spinlock.h"

/* ── SDHCI Standard MMIO Registers ────────────────────────────────────────── */
#define SDHCI_DMA_ADDRESS          0x00
#define SDHCI_BLOCK_SIZE           0x04
#define SDHCI_BLOCK_COUNT          0x06
#define SDHCI_ARGUMENT             0x08
#define SDHCI_TRANSFER_MODE        0x0C
#define SDHCI_COMMAND              0x0E
#define SDHCI_RESPONSE_0           0x10
#define SDHCI_RESPONSE_1           0x14
#define SDHCI_RESPONSE_2           0x18
#define SDHCI_RESPONSE_3           0x1C
#define SDHCI_BUFFER               0x20
#define SDHCI_PRESENT_STATE        0x24
#define SDHCI_HOST_CONTROL         0x28
#define SDHCI_POWER_CONTROL        0x29
#define SDHCI_BLOCK_GAP_CONTROL    0x2A
#define SDHCI_WAKE_UP_CONTROL      0x2B
#define SDHCI_CLOCK_CONTROL        0x2C
#define SDHCI_TIMEOUT_CONTROL      0x2E
#define SDHCI_SOFTWARE_RESET       0x2F
#define SDHCI_INT_STATUS           0x30
#define SDHCI_ERR_INT_STATUS       0x32
#define SDHCI_INT_ENABLE           0x34
#define SDHCI_ERR_INT_ENABLE       0x36
#define SDHCI_SIGNAL_ENABLE        0x38
#define SDHCI_ERR_SIGNAL_ENABLE    0x3A
#define SDHCI_ACMD12_ERR_STATUS    0x3C
#define SDHCI_HOST_CONTROL2        0x3E
#define SDHCI_CAPABILITIES         0x40
#define SDHCI_CAPABILITIES_1       0x44
#define SDHCI_MAX_CURRENT          0x48
#define SDHCI_HOST_VERSION         0xFE

/* Present State Register bits */
#define SDHCI_CMD_INHIBIT          (1U << 0)
#define SDHCI_DATA_INHIBIT         (1U << 1)
#define SDHCI_DAT_ACTIVE           (1U << 2)
#define SDHCI_WRITE_PROTECT        (1U << 19)
#define SDHCI_CARD_DETECT_PIN      (1U << 18)
#define SDHCI_CARD_STABLE          (1U << 17)
#define SDHCI_CARD_PRESENT         (1U << 16)
#define SDHCI_BUFFER_READ_ENABLE   (1U << 11)
#define SDHCI_BUFFER_WRITE_ENABLE  (1U << 10)

/* Software Reset Register bits */
#define SDHCI_RESET_ALL            0x01
#define SDHCI_RESET_CMD            0x02
#define SDHCI_RESET_DATA           0x04

/* Power Control bits */
#define SDHCI_POWER_ON             0x01
#define SDHCI_POWER_330            (0x07 << 1)
#define SDHCI_POWER_300            (0x06 << 1)
#define SDHCI_POWER_180            (0x05 << 1)

/* Clock Control bits */
#define SDHCI_CLOCK_INTERNAL_EN    0x01
#define SDHCI_CLOCK_STABLE         0x02
#define SDHCI_CLOCK_SD_EN          0x04

/* Interrupt Status / Enable bits */
#define SDHCI_INT_CMD_COMPLETE     (1U << 0)
#define SDHCI_INT_XFER_COMPLETE    (1U << 1)
#define SDHCI_INT_BLOCK_GAP        (1U << 2)
#define SDHCI_INT_DMA              (1U << 3)
#define SDHCI_INT_BUF_WRITE_READY  (1U << 4)
#define SDHCI_INT_BUF_READ_READY   (1U << 5)
#define SDHCI_INT_CARD_INSERT      (1U << 6)
#define SDHCI_INT_CARD_REMOVE      (1U << 7)
#define SDHCI_INT_CARD_INT         (1U << 8)
#define SDHCI_INT_ERROR            (1U << 15)

/* Transfer Mode bits */
#define SDHCI_TRNS_DMA             0x01
#define SDHCI_TRNS_BLK_CNT_EN      0x02
#define SDHCI_TRNS_AUTO_CMD12      0x04
#define SDHCI_TRNS_READ            0x10
#define SDHCI_TRNS_MULTI           0x20

/* Command Register bits */
#define SDHCI_CMD_RESP_NONE        0x00
#define SDHCI_CMD_RESP_136         0x01
#define SDHCI_CMD_RESP_48          0x02
#define SDHCI_CMD_RESP_48_BUSY     0x03
#define SDHCI_CMD_CRC_CHECK        0x08
#define SDHCI_CMD_INDEX_CHECK      0x10
#define SDHCI_CMD_DATA_PRESENT     0x20

/* SD Card Standard Commands */
#define MMC_GO_IDLE_STATE          0
#define MMC_ALL_SEND_CID           2
#define MMC_SET_RELATIVE_ADDR      3
#define MMC_SELECT_CARD            7
#define MMC_SEND_IF_COND           8
#define MMC_SEND_CSD               9
#define MMC_STOP_TRANSMISSION      12
#define MMC_SET_BLOCKLEN           16
#define MMC_READ_SINGLE_BLOCK      17
#define MMC_READ_MULTIPLE_BLOCK    18
#define MMC_WRITE_BLOCK            24
#define MMC_WRITE_MULTIPLE_BLOCK   25
#define MMC_APP_CMD                55
#define SD_APP_SET_BUS_WIDTH       6
#define SD_APP_OP_COND             41

/* SD Card Types */
#define SD_TYPE_UNKNOWN            0
#define SD_TYPE_STANDARD           1   /* SDSC (byte-addressed) */
#define SD_TYPE_HIGH_CAPACITY      2   /* SDHC / SDXC (block-addressed) */

typedef struct sdhci_host {
    uintptr_t       mmio_base;
    u32             mmio_size;
    u16             rca;
    u8              card_type;
    bool            card_inserted;
    u64             capacity_bytes;
    u32             sector_size;
    u64             sector_count;
    block_dev_t     bdev;
    spinlock_t      lock;
} sdhci_host_t;

/** sdhci_init() — Probe PCI bus for SD/MMC Host Controllers and initialize cards. */
void sdhci_init(void);
