/* ============================================================================
 * AzamiOS — Intel PRO/100 (i8255x / e100) Fast Ethernet Driver Header
 * File: drivers/net/e100.h
 * ============================================================================ */
#pragma once

#include "../../include/azami/defs.h"
#include "../../include/azami/net.h"
#include "../../hal/pci.h"
#include "../base/pci_bus.h"

/* ── SCB Register Offsets (CSR) ─────────────────────────────────────────── */
#define E100_SCB_STATUS         0x00  /* 16-bit SCB Status Word */
#define E100_SCB_CMD            0x02  /* 16-bit SCB Command Word */
#define E100_SCB_POINTER        0x04  /* 32-bit SCB General Pointer */
#define E100_SCB_PORT           0x08  /* 32-bit PORT Interface */
#define E100_SCB_FLASH_CTRL     0x0C  /* 16-bit Flash Control */
#define E100_SCB_EEPROM_CTRL    0x0E  /* 16-bit EEPROM Control */
#define E100_SCB_MDI_CTRL       0x10  /* 32-bit MDI Control */
#define E100_SCB_RX_BC          0x14  /* 32-bit Early Rx Byte Count */

/* ── SCB Status Word Bits ────────────────────────────────────────────────── */
#define E100_STATUS_CX          (1 << 15) /* Command unit executed */
#define E100_STATUS_FR          (1 << 14) /* Frame received */
#define E100_STATUS_CNA         (1 << 13) /* CU not active */
#define E100_STATUS_RNR         (1 << 12) /* RU not ready */
#define E100_STATUS_M           (1 << 11) /* Mask */
#define E100_STATUS_SWI         (1 << 10) /* Software generated interrupt */

/* ── SCB Command Word Bits ───────────────────────────────────────────────── */
#define E100_CU_NOP             0x0000
#define E100_CU_START           0x0010
#define E100_CU_RESUME          0x0020
#define E100_CU_LOAD_DUMP_ADDR  0x0040
#define E100_CU_DUMP_STAT       0x0050
#define E100_CU_LOAD_BASE       0x0060
#define E100_CU_DUMP_RESET      0x0070

#define E100_RU_NOP             0x0000
#define E100_RU_START           0x0001
#define E100_RU_RESUME          0x0002
#define E100_RU_DMA_REDIRECT    0x0003
#define E100_RU_ABORT           0x0004
#define E100_RU_LOAD_HDS        0x0005
#define E100_RU_LOAD_BASE       0x0006

#define E100_INT_MASK           0x0100

/* ── PORT Commands ───────────────────────────────────────────────────────── */
#define E100_PORT_SOFTWARE_RESET  0x00000000
#define E100_PORT_SELFTEST        0x00000001
#define E100_PORT_SELECTIVE_RESET 0x00000002

/* ── EEPROM Control Register Bits ────────────────────────────────────────── */
#define E100_EEPROM_EEDO        (1 << 3) /* Serial Data Out */
#define E100_EEPROM_EEDI        (1 << 2) /* Serial Data In */
#define E100_EEPROM_EESK        (1 << 1) /* Serial Clock */
#define E100_EEPROM_EECS        (1 << 0) /* Chip Select */

/* ── Tx Command Block (TxCB) ─────────────────────────────────────────────── */
#define E100_CB_CMD_TX          0x0004
#define E100_CB_CMD_EL          0x8000
#define E100_CB_CMD_S           0x4000
#define E100_CB_CMD_I           0x2000

#define E100_CB_STATUS_C        0x8000
#define E100_CB_STATUS_OK       0x2000

typedef struct __attribute__((packed, aligned(4))) e100_tx_cb {
    volatile u16 status;
    volatile u16 command;
    volatile u32 link;
    volatile u32 tbd_array_addr;
    volatile u16 byte_count;
    volatile u8  tx_threshold;
    volatile u8  tbd_count;
    volatile u8  data[1536];
} e100_tx_cb_t;

/* ── Receive Frame Descriptor (RFD) ──────────────────────────────────────── */
#define E100_RFD_STATUS_C       0x8000
#define E100_RFD_STATUS_OK      0x2000

#define E100_RFD_CMD_EL         0x8000
#define E100_RFD_CMD_S          0x4000

#define E100_RFD_CNT_EOF        0x8000
#define E100_RFD_CNT_F          0x4000

#define E100_NUM_RFD            16

typedef struct __attribute__((packed, aligned(4))) e100_rfd {
    volatile u16 status;
    volatile u16 command;
    volatile u32 link;
    volatile u32 reserved;
    volatile u16 actual_count;
    volatile u16 size;
    volatile u8  data[1536];
} e100_rfd_t;

/* ── e100 Driver State ───────────────────────────────────────────────────── */
typedef struct e100_device {
    virt_addr_t   mmio_base;
    phys_addr_t   mmio_phys;
    u16           io_base;
    bool          use_io;
    u8            irq;
    u8            mac[6];

    /* Transmit buffer */
    e100_tx_cb_t *tx_cb;
    phys_addr_t   tx_cb_phys;

    /* Receive ring */
    e100_rfd_t   *rfd_ring[E100_NUM_RFD];
    phys_addr_t   rfd_phys[E100_NUM_RFD];
    u32           rx_cur;

    bool          ready;
    net_device_t  netdev;
} e100_device_t;

/** e100_init() — Register the Intel PRO/100 PCI driver. */
int e100_init(void);

/** Retrieve current MAC address. */
void e100_get_mac(u8 mac_out[6]);
