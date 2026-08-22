/* ============================================================================
 * AzamiOS — Dynamic ARP Cache & Protocol Header (arp.h)
 * File: include/azami/arp.h
 *
 * Implements RFC 826 Address Resolution Protocol, dynamic ARP caching,
 * pending packet resolution queues, and timer expiration.
 * ============================================================================ */
#pragma once

#include "types.h"
#include "net_buf.h"

#define ARP_TABLE_SIZE     64
#define ARP_ENTRY_TIMEOUT  300 /* 300 seconds TTL for resolved entries */
#define ARP_RETRY_TIMEOUT  1   /* 1 second between request retries     */
#define ARP_MAX_RETRIES    5   /* Max retransmissions before drop      */

#define ARP_STATE_FREE     0
#define ARP_STATE_PENDING  1
#define ARP_STATE_RESOLVED 2

typedef struct {
    u8              ip[4];
    u8              mac[6];
    u8              state;
    u8              retries;
    u32             timestamp;
    net_buf_queue_t pending_queue;
} arp_entry_t;

/* Public ARP API */
void arp_init(void);
int  arp_resolve(const u8 ip[4], u8 mac_out[6], net_buf_t *pending_buf);
void arp_input(const u8 *pkt, size_t len);
void arp_send_request(const u8 target_ip[4]);
void arp_send_reply(const u8 target_ip[4], const u8 target_mac[6]);
void arp_timer_tick(void);
void arp_print_table(void);
