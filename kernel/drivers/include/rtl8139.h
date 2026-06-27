/**
 * rtl8139.h  –  Realtek RTL8139 10/100 Mbps Fast Ethernet Driver Header
 */
#ifndef RTL8139_H
#define RTL8139_H

#include <stdint.h>
#include <stdbool.h>

#define RTL8139_VENDOR_ID 0x10EC
#define RTL8139_DEVICE_ID 0x8139

/* RTL8139 Register Offsets (I/O Space) */
#define RTL_IDR0       0x00    /* MAC Address registers (0..5) */
#define RTL_TSD0       0x10    /* Transmit Status (0..3) */
#define RTL_TSAD0      0x20    /* Transmit Start Address (0..3) */
#define RTL_RBSTART    0x30    /* Receive Buffer Start Address */
#define RTL_CR         0x37    /* Command Register */
#define RTL_CAPR       0x38    /* Current Address of Packet Read */
#define RTL_CBR        0x3A    /* Current Buffer Address */
#define RTL_IMR        0x3C    /* Interrupt Mask Register */
#define RTL_ISR        0x3E    /* Interrupt Status Register */
#define RTL_TCR        0x40    /* Transmit Configuration Register */
#define RTL_RCR        0x44    /* Receive Configuration Register */
#define RTL_CONFIG1    0x52    /* Configuration Register 1 */

/* Command Register Bits */
#define RTL_CR_BUFE    (1 << 0)
#define RTL_CR_TE      (1 << 2)
#define RTL_CR_RE      (1 << 3)
#define RTL_CR_RST     (1 << 4)

/* Interrupt Mask / Status Bits */
#define RTL_INT_ROK    (1 << 0) /* Receive OK */
#define RTL_INT_RER    (1 << 1) /* Receive Error */
#define RTL_INT_TOK    (1 << 2) /* Transmit OK */
#define RTL_INT_TER    (1 << 3) /* Transmit Error */
#define RTL_INT_RXOVW  (1 << 4) /* Rx Buffer Overflow */

/* Receive Configuration Bits */
#define RTL_RCR_AAP    (1 << 0) /* Accept All Packets (Promiscuous) */
#define RTL_RCR_APM    (1 << 1) /* Accept Physical Match */
#define RTL_RCR_AM     (1 << 2) /* Accept Multicast */
#define RTL_RCR_AB     (1 << 3) /* Accept Broadcast */
#define RTL_RCR_WRAP   (1 << 7) /* Rx Buffer Wrap */

#define RTL_RX_BUF_SIZE 8192    /* 8K standard buffer + wrap area */

/* Driver state */
typedef struct {
    uint8_t  mac[6];
    uint16_t io_base;
    uint8_t  irq;
    bool     present;
    uint32_t rx_buffer_phys;
    uint32_t tx_buffers_phys[4];
    uint8_t  cur_tx;
    uint32_t cur_rx;
    uint32_t rx_packets;
    uint32_t tx_packets;
} rtl8139_dev_t;

void rtl8139_init(void);
void rtl8139_send_packet(const uint8_t *data, uint32_t len);
void rtl8139_print_status(void);
void rtl8139_send_test_packet(void);
bool rtl8139_is_enabled(void);
rtl8139_dev_t *rtl8139_get_device(void);

#endif /* RTL8139_H */
