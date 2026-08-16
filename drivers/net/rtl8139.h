/* ============================================================================
 * AzamiOS — Realtek RTL8139 Fast Ethernet NIC Driver Header
 * File: drivers/net/rtl8139.h
 * ============================================================================ */
#pragma once

#include "../../include/azami/types.h"

int  rtl8139_init(void);
s64  rtl8139_send_packet(const void *data, size_t len);
s64  rtl8139_recv_packet(void *buf, size_t max_len);
void rtl8139_get_mac(u8 mac_out[6]);
