/* ============================================================================
 * AzamiOS — Realtek RTL8169 / RTL8168 Gigabit Ethernet Driver
 * File: drivers/net/rtl8169.h
 * ============================================================================ */
#pragma once

#include "../../include/azami/types.h"

#define RTL8169_NUM_RX_DESC   32
#define RTL8169_NUM_TX_DESC   32
#define RTL8169_PKT_BUF_SIZE  2048

/* Descriptor bits */
#define RTL8169_DESC_OWN      (1U << 31)  /* 1 = HW owns, 0 = SW owns */
#define RTL8169_DESC_EOR      (1U << 30)  /* End of Ring (wrap)       */
#define RTL8169_DESC_FS       (1U << 29)  /* First segment descriptor */
#define RTL8169_DESC_LS       (1U << 28)  /* Last segment descriptor  */

typedef struct __attribute__((packed, aligned(16))) {
    u32 opts1;
    u32 opts2;
    u64 buf_addr;
} rtl8169_desc_t;

void rtl8169_init(void);
void rtl8169_get_mac(u8 mac_out[6]);
s64  rtl8169_send_packet(const void *data, size_t len);
s64  rtl8169_recv_packet(void *buf, size_t max_len);
void rtl8169_poll_rx(void);
