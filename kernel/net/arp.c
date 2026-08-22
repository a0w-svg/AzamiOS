/* ============================================================================
 * AzamiOS — Dynamic ARP Cache & Resolution Protocol (arp.c)
 * File: kernel/net/arp.c
 *
 * Implements RFC 826 ARP state machine, resolution queues, cache expiration,
 * and Ethernet frame dispatching.
 * ============================================================================ */

#define DEBUG 1
#include "../../include/azami/debug.h"
#include "../../include/azami/defs.h"
#include "../../include/azami/net.h"
#include "../../include/azami/arp.h"
#include "../../arch/x86_64/cpu/spinlock.h"
#include "../../kernel/lib/string.h"

static arp_entry_t g_arp_table[ARP_TABLE_SIZE];
static spinlock_t  g_arp_lock = SPINLOCK_INIT;
static u32         g_arp_ticks = 0;

static inline u16 htons(u16 v) { return (u16)((v << 8) | (v >> 8)); }
static inline u16 ntohs(u16 v) { return htons(v); }

void arp_init(void)
{
    spinlock_lock(&g_arp_lock);
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        memset(&g_arp_table[i], 0, sizeof(arp_entry_t));
        g_arp_table[i].state = ARP_STATE_FREE;
        net_buf_queue_init(&g_arp_table[i].pending_queue);
    }
    spinlock_unlock(&g_arp_lock);
    pr_debug("[ARP] Dynamic ARP cache subsystem initialized (%d entries).\n", ARP_TABLE_SIZE);
}

void arp_send_request(const u8 target_ip[4])
{
    u8 host_ip[4];
    u8 host_mac[6];
    net_get_ip(host_ip);
    net_device_t *dev = net_get_default_device();
    if (!dev) return;
    memcpy(host_mac, dev->mac, 6);

    u8 pkt[sizeof(eth_hdr_t) + sizeof(arp_pkt_t)];
    eth_hdr_t *eth = (eth_hdr_t *)pkt;
    arp_pkt_t *arp = (arp_pkt_t *)(pkt + sizeof(eth_hdr_t));

    /* Ethernet Header: Broadcast */
    memset(eth->dst, 0xFF, 6);
    memcpy(eth->src, host_mac, 6);
    eth->ethertype = htons(ETH_P_ARP);

    /* ARP Request */
    arp->htype = htons(1);         /* Ethernet */
    arp->ptype = htons(ETH_P_IP);  /* IPv4 */
    arp->hlen = 6;
    arp->plen = 4;
    arp->oper = htons(ARP_OP_REQUEST);
    memcpy(arp->sha, host_mac, 6);
    memcpy(arp->spa, host_ip, 4);
    memset(arp->tha, 0x00, 6);
    memcpy(arp->tpa, target_ip, 4);

    dev->send(pkt, sizeof(pkt));
}

void arp_send_reply(const u8 target_ip[4], const u8 target_mac[6])
{
    u8 host_ip[4];
    u8 host_mac[6];
    net_get_ip(host_ip);
    net_device_t *dev = net_get_default_device();
    if (!dev) return;
    memcpy(host_mac, dev->mac, 6);

    u8 pkt[sizeof(eth_hdr_t) + sizeof(arp_pkt_t)];
    eth_hdr_t *eth = (eth_hdr_t *)pkt;
    arp_pkt_t *arp = (arp_pkt_t *)(pkt + sizeof(eth_hdr_t));

    /* Ethernet Header: Unicast */
    memcpy(eth->dst, target_mac, 6);
    memcpy(eth->src, host_mac, 6);
    eth->ethertype = htons(ETH_P_ARP);

    /* ARP Reply */
    arp->htype = htons(1);
    arp->ptype = htons(ETH_P_IP);
    arp->hlen = 6;
    arp->plen = 4;
    arp->oper = htons(ARP_OP_REPLY);
    memcpy(arp->sha, host_mac, 6);
    memcpy(arp->spa, host_ip, 4);
    memcpy(arp->tha, target_mac, 6);
    memcpy(arp->tpa, target_ip, 4);

    dev->send(pkt, sizeof(pkt));
}

int arp_resolve(const u8 ip[4], u8 mac_out[6], net_buf_t *pending_buf)
{
    if (!ip || !mac_out) return -1;

    /* Local IP check */
    u8 host_ip[4];
    net_get_ip(host_ip);
    if (memcmp(ip, host_ip, 4) == 0) {
        net_device_t *dev = net_get_default_device();
        if (dev) {
            memcpy(mac_out, dev->mac, 6);
            return 0;
        }
    }

    /* Broadcast check */
    if (ip[0] == 255 && ip[1] == 255 && ip[2] == 255 && ip[3] == 255) {
        memset(mac_out, 0xFF, 6);
        return 0;
    }

    spinlock_lock(&g_arp_lock);

    /* 1. Search existing entries */
    int free_slot = -1;
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        if (g_arp_table[i].state != ARP_STATE_FREE) {
            if (memcmp(g_arp_table[i].ip, ip, 4) == 0) {
                if (g_arp_table[i].state == ARP_STATE_RESOLVED) {
                    memcpy(mac_out, g_arp_table[i].mac, 6);
                    spinlock_unlock(&g_arp_lock);
                    return 0;
                } else if (g_arp_table[i].state == ARP_STATE_PENDING) {
                    if (pending_buf) {
                        net_buf_queue_push(&g_arp_table[i].pending_queue, pending_buf);
                    }
                    spinlock_unlock(&g_arp_lock);
                    return -1;
                }
            }
        } else if (free_slot < 0) {
            free_slot = i;
        }
    }

    /* 2. Allocate new entry */
    if (free_slot < 0) free_slot = 0; /* evict first entry */

    arp_entry_t *entry = &g_arp_table[free_slot];
    net_buf_queue_purge(&entry->pending_queue);

    memcpy(entry->ip, ip, 4);
    memset(entry->mac, 0, 6);
    entry->state = ARP_STATE_PENDING;
    entry->retries = 1;
    entry->timestamp = g_arp_ticks;

    if (pending_buf) {
        net_buf_queue_push(&entry->pending_queue, pending_buf);
    }

    spinlock_unlock(&g_arp_lock);

    /* Transmit initial ARP request broadcast */
    arp_send_request(ip);

    return -1;
}

void arp_input(const u8 *pkt, size_t len)
{
    if (!pkt || len < sizeof(eth_hdr_t) + sizeof(arp_pkt_t)) return;

    const arp_pkt_t *arp = (const arp_pkt_t *)(pkt + sizeof(eth_hdr_t));
    u16 oper = ntohs(arp->oper);

    if (ntohs(arp->htype) != 1 || ntohs(arp->ptype) != ETH_P_IP) return;

    u8 host_ip[4];
    net_get_ip(host_ip);

    spinlock_lock(&g_arp_lock);

    /* Update or insert sender IP into ARP cache */
    arp_entry_t *match = NULL;
    int free_slot = -1;

    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        if (g_arp_table[i].state != ARP_STATE_FREE) {
            if (memcmp(g_arp_table[i].ip, arp->spa, 4) == 0) {
                match = &g_arp_table[i];
                break;
            }
        } else if (free_slot < 0) {
            free_slot = i;
        }
    }

    if (!match && free_slot >= 0) {
        match = &g_arp_table[free_slot];
        memcpy(match->ip, arp->spa, 4);
    }

    net_device_t *dev = net_get_default_device();

    if (match) {
        memcpy(match->mac, arp->sha, 6);
        match->state = ARP_STATE_RESOLVED;
        match->timestamp = g_arp_ticks;
        match->retries = 0;

        /* Flush all pending queued packets for this IP address */
        net_buf_t *pending = NULL;
        while ((pending = net_buf_queue_pop(&match->pending_queue)) != NULL) {
            if (dev && pending->len >= sizeof(eth_hdr_t)) {
                eth_hdr_t *eth_hdr = (eth_hdr_t *)pending->data;
                memcpy(eth_hdr->dst, match->mac, 6);
                dev->send(pending->data, pending->len);
            }
            net_buf_free(pending);
        }
    }

    spinlock_unlock(&g_arp_lock);

    /* If this is an ARP request asking for our IP address, send an ARP reply */
    if (oper == ARP_OP_REQUEST) {
        if (memcmp(arp->tpa, host_ip, 4) == 0) {
            arp_send_reply(arp->spa, arp->sha);
        }
    }
}

void arp_timer_tick(void)
{
    spinlock_lock(&g_arp_lock);
    g_arp_ticks++;

    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        arp_entry_t *e = &g_arp_table[i];
        if (e->state == ARP_STATE_RESOLVED) {
            if (g_arp_ticks - e->timestamp > ARP_ENTRY_TIMEOUT) {
                /* Expire stale entry */
                e->state = ARP_STATE_FREE;
                net_buf_queue_purge(&e->pending_queue);
            }
        } else if (e->state == ARP_STATE_PENDING) {
            if (g_arp_ticks - e->timestamp >= ARP_RETRY_TIMEOUT) {
                if (e->retries < ARP_MAX_RETRIES) {
                    e->retries++;
                    e->timestamp = g_arp_ticks;
                    arp_send_request(e->ip);
                } else {
                    /* Max retries reached without response, drop */
                    e->state = ARP_STATE_FREE;
                    net_buf_queue_purge(&e->pending_queue);
                }
            }
        }
    }

    spinlock_unlock(&g_arp_lock);
}

void arp_print_table(void)
{
    spinlock_lock(&g_arp_lock);
    pr_debug("=== ARP Cache Table ===\n");
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        if (g_arp_table[i].state == ARP_STATE_RESOLVED) {
            pr_debug("  %u.%u.%u.%u -> %02x:%02x:%02x:%02x:%02x:%02x (age: %us)\n",
                     g_arp_table[i].ip[0], g_arp_table[i].ip[1],
                     g_arp_table[i].ip[2], g_arp_table[i].ip[3],
                     g_arp_table[i].mac[0], g_arp_table[i].mac[1],
                     g_arp_table[i].mac[2], g_arp_table[i].mac[3],
                     g_arp_table[i].mac[4], g_arp_table[i].mac[5],
                     g_arp_ticks - g_arp_table[i].timestamp);
        }
    }
    spinlock_unlock(&g_arp_lock);
}
