/* ============================================================================
 * AzamiOS — Intel 8254x / 82574L (e1000 / e1000e) Gigabit Ethernet Driver Header
 * File: drivers/net/e1000.h
 * ============================================================================ */
#pragma once

#include "../../include/azami/types.h"
#include "../../fs/vfs.h"

#define E1000_NUM_RX_DESC 32
#define E1000_NUM_TX_DESC 32
#define E1000_PKT_BUF_SIZE 2048

/* ── Registers ───────────────────────────────────────────────────────────── */
#define E1000_CTRL      0x0000  /* Device Control */
#define E1000_STATUS    0x0008  /* Device Status */
#define E1000_EECD      0x0010  /* EEPROM Control */
#define E1000_EERD      0x0014  /* EEPROM Read */
#define E1000_ICR       0x00C0  /* Interrupt Cause Read */
#define E1000_IMS       0x00D0  /* Interrupt Mask Set */
#define E1000_IMC       0x00D8  /* Interrupt Mask Clear */
#define E1000_RCTL      0x0100  /* Receive Control */
#define E1000_TCTL      0x0400  /* Transmit Control */
#define E1000_TIPG      0x0410  /* Transmit IPG */
#define E1000_RDBAL     0x2800  /* RX Descriptor Base Address Low */
#define E1000_RDBAH     0x2804  /* RX Descriptor Base Address High */
#define E1000_RDLEN     0x2808  /* RX Descriptor Length */
#define E1000_RDH       0x2810  /* RX Descriptor Head */
#define E1000_RDT       0x2818  /* RX Descriptor Tail */
#define E1000_TDBAL     0x3800  /* TX Descriptor Base Address Low */
#define E1000_TDBAH     0x3804  /* TX Descriptor Base Address High */
#define E1000_TDLEN     0x3808  /* TX Descriptor Length */
#define E1000_TDH       0x3810  /* TX Descriptor Head */
#define E1000_TDT       0x3818  /* TX Descriptor Tail */
#define E1000_MTA       0x5200  /* Multicast Table Array */
#define E1000_RAL       0x5400  /* Receive Address Low */
#define E1000_RAH       0x5404  /* Receive Address High */

/* ── Control Register Bits ───────────────────────────────────────────────── */
#define E1000_CTRL_SLU   (1 << 6)   /* Set Link Up */
#define E1000_CTRL_RST   (1 << 26)  /* Device Reset */

/* ── Receive Control Bits ────────────────────────────────────────────────── */
#define E1000_RCTL_EN    (1 << 1)   /* Receiver Enable */
#define E1000_RCTL_SBP   (1 << 2)   /* Store Bad Packets */
#define E1000_RCTL_UPE   (1 << 3)   /* Unicast Promiscuous Enable */
#define E1000_RCTL_MPE   (1 << 4)   /* Multicast Promiscuous Enable */
#define E1000_RCTL_BAM   (1 << 15)  /* Broadcast Accept Mode */
#define E1000_RCTL_BSIZE_2048 (0 << 16) /* Buffer Size = 2048 B */
#define E1000_RCTL_SECRC (1 << 26)  /* Strip Ethernet CRC */

/* ── Transmit Control Bits ───────────────────────────────────────────────── */
#define E1000_TCTL_EN    (1 << 1)   /* Transmitter Enable */
#define E1000_TCTL_PSP   (1 << 3)   /* Pad Short Packets */
#define E1000_TCTL_CT_SHIFT 4       /* Collision Threshold */
#define E1000_TCTL_COLD_SHIFT 12    /* Collision Distance */

/* ── Interrupt Bits (ICR / IMS / IMC) ─────────────────────────────────────── */
#define E1000_ICR_TXDW   (1 << 0)
#define E1000_ICR_TXQE   (1 << 1)
#define E1000_ICR_LSC    (1 << 2)
#define E1000_ICR_RXSEQ  (1 << 3)
#define E1000_ICR_RXDMT0 (1 << 4)
#define E1000_ICR_RXO    (1 << 6)
#define E1000_ICR_RXT0   (1 << 7)

#define E1000_IMS_TXDW   (1 << 0)
#define E1000_IMS_TXQE   (1 << 1)
#define E1000_IMS_LSC    (1 << 2)
#define E1000_IMS_RXSEQ  (1 << 3)
#define E1000_IMS_RXDMT0 (1 << 4)
#define E1000_IMS_RXO    (1 << 6)
#define E1000_IMS_RXT0   (1 << 7)

/* ── Descriptors ─────────────────────────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    u64 addr;
    u16 length;
    u16 checksum;
    u8  status;
    u8  errors;
    u16 special;
} e1000_rx_desc_t;

#define E1000_RXD_STAT_DD  (1 << 0) /* Descriptor Done */
#define E1000_RXD_STAT_EOP (1 << 1) /* End of Packet */

typedef struct __attribute__((packed)) {
    u64 addr;
    u16 length;
    u8  cso;
    u8  cmd;
    u8  status;
    u8  css;
    u16 special;
} e1000_tx_desc_t;

#define E1000_TXD_CMD_EOP  (1 << 0) /* End of Packet */
#define E1000_TXD_CMD_IFCS (1 << 1) /* Insert FCS */
#define E1000_TXD_CMD_RS   (1 << 3) /* Report Status */
#define E1000_TXD_STAT_DD  (1 << 0) /* Descriptor Done */

/* ── Device Structure ────────────────────────────────────────────────────── */
typedef struct {
    u8  mac[6];
    u32 mmio_base;
    u8  irq;
    bool has_eeprom;
    bool link_up;

    e1000_rx_desc_t *rx_descs;
    phys_addr_t      rx_descs_phys;
    u8              *rx_buffers[E1000_NUM_RX_DESC];
    u32              rx_cur;

    e1000_tx_desc_t *tx_descs;
    phys_addr_t      tx_descs_phys;
    u8              *tx_buffers[E1000_NUM_TX_DESC];
    u32              tx_cur;
} e1000_device_t;

/* Public API */
int  e1000_init(void);
s64  e1000_send_packet(const void *data, size_t len);
s64  e1000_recv_packet(void *buf, size_t max_len);
void e1000_poll_rx(void);
void e1000_get_mac(u8 mac_out[6]);
bool e1000_is_link_up(void);
