/* ============================================================================
 * AzamiOS — AMD PCnet-FAST III (Am79C973) Ethernet Driver Header
 * File: drivers/net/pcnet.h
 * ============================================================================ */
#pragma once

#include "../../include/azami/types.h"
#include "../../include/azami/defs.h"
#include "../../hal/device.h"

#define PCNET_VENDOR_ID  0x1022
#define PCNET_DEVICE_ID  0x2000

/** pcnet_init() — Probe and initialize AMD PCnet-FAST III network controller. */
int pcnet_init(device_t *dev);

/** pcnet_send_packet() — Transmit ethernet frame. */
s64 pcnet_send_packet(const void *data, size_t len);

/** pcnet_recv_packet() — Receive ethernet frame. */
s64 pcnet_recv_packet(void *buf, size_t max_len);

/** pcnet_get_mac() — Retrieve MAC address. */
void pcnet_get_mac(u8 mac[6]);
