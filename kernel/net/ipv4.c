/* ============================================================================
 * AzamiOS — IPv4 Protocol Engine & Routing Subsystem (ipv4.c)
 * File: kernel/net/ipv4.c
 *
 * Implements RFC 791 IPv4 protocol engine, subnet routing table,
 * longest-prefix matching, and packet encapsulation.
 * ============================================================================ */

#define DEBUG 1
#include "../../include/azami/debug.h"
#include "../../include/azami/defs.h"
#include "../../include/azami/net.h"
#include "../../include/azami/ipv4.h"
#include "../../include/azami/icmp.h"
#include "../../include/azami/arp.h"
#include "../../arch/x86_64/cpu/spinlock.h"
#include "../../kernel/lib/string.h"

static route_entry_t g_routes[MAX_ROUTES];
static spinlock_t    g_route_lock = SPINLOCK_INIT;
static u16        g_ip_id_counter = 1;

static inline u16 htons(u16 v) { return (u16)((v << 8) | (v >> 8)); }
static inline u16 ntohs(u16 v) { return htons(v); }

/* ── Routing Table Subsystem ──────────────────────────────────────────────── */

void route_init(void)
{
    spinlock_lock(&g_route_lock);
    for (int i = 0; i < MAX_ROUTES; i++) {
        memset(&g_routes[i], 0, sizeof(route_entry_t));
    }

    net_device_t *dev = net_get_default_device();

    /* 1. Local loopback / Host route (127.0.0.1/8) */
    static const u8 loop_dst[4] = { 127, 0, 0, 0 };
    static const u8 loop_mask[4] = { 255, 0, 0, 0 };
    static const u8 zero_gw[4] = { 0, 0, 0, 0 };
    memcpy(g_routes[0].dst, loop_dst, 4);
    memcpy(g_routes[0].mask, loop_mask, 4);
    memcpy(g_routes[0].gateway, zero_gw, 4);
    g_routes[0].dev = dev;
    g_routes[0].flags = RT_FLAG_UP | RT_FLAG_HOST;
    g_routes[0].metric = 0;

    spinlock_unlock(&g_route_lock);
    pr_debug("[IPv4] Routing table initialized with local loopback.\n");
}

int route_add(const u8 dst[4], const u8 mask[4], const u8 gw[4], struct net_device *dev, u32 flags, u32 metric)
{
    if (!dst || !mask) return -1;
    spinlock_lock(&g_route_lock);

    int target_slot = -1;
    int free_slot = -1;
    for (int i = 0; i < MAX_ROUTES; i++) {
        if (g_routes[i].flags & RT_FLAG_UP) {
            if (memcmp(g_routes[i].dst, dst, 4) == 0 &&
                memcmp(g_routes[i].mask, mask, 4) == 0) {
                target_slot = i;
                break;
            }
        } else if (free_slot < 0) {
            free_slot = i;
        }
    }

    int slot = (target_slot >= 0) ? target_slot : free_slot;
    if (slot < 0) {
        spinlock_unlock(&g_route_lock);
        return -1; /* Table full */
    }

    memcpy(g_routes[slot].dst, dst, 4);
    memcpy(g_routes[slot].mask, mask, 4);
    if (gw) memcpy(g_routes[slot].gateway, gw, 4);
    else memset(g_routes[slot].gateway, 0, 4);
    g_routes[slot].dev = dev ? dev : net_get_default_device();
    g_routes[slot].flags = flags | RT_FLAG_UP;
    g_routes[slot].metric = metric;

    spinlock_unlock(&g_route_lock);
    return 0;
}

int route_del(const u8 dst[4], const u8 mask[4])
{
    if (!dst || !mask) return -1;
    spinlock_lock(&g_route_lock);

    for (int i = 0; i < MAX_ROUTES; i++) {
        if (g_routes[i].flags & RT_FLAG_UP) {
            if (memcmp(g_routes[i].dst, dst, 4) == 0 &&
                memcmp(g_routes[i].mask, mask, 4) == 0) {
                g_routes[i].flags = 0;
                spinlock_unlock(&g_route_lock);
                return 0;
            }
        }
    }

    spinlock_unlock(&g_route_lock);
    return -1;
}

int route_lookup(const u8 dst_ip[4], u8 next_hop_out[4], struct net_device **dev_out)
{
    if (!dst_ip || !next_hop_out || !dev_out) return -1;

    spinlock_lock(&g_route_lock);

    int best_match = -1;
    int best_prefix_len = -1;
    u32 best_metric = 0xFFFFFFFF;

    for (int i = 0; i < MAX_ROUTES; i++) {
        if (!(g_routes[i].flags & RT_FLAG_UP)) continue;

        /* Check if destination IP matches this subnet route */
        bool match = true;
        int prefix_len = 0;
        for (int k = 0; k < 4; k++) {
            if ((dst_ip[k] & g_routes[i].mask[k]) != g_routes[i].dst[k]) {
                match = false;
                break;
            }
            /* Count 1-bits in mask */
            u8 m = g_routes[i].mask[k];
            while (m) {
                if (m & 0x80) prefix_len++;
                m <<= 1;
            }
        }

        if (match) {
            if (prefix_len > best_prefix_len ||
                (prefix_len == best_prefix_len && g_routes[i].metric < best_metric)) {
                best_match = i;
                best_prefix_len = prefix_len;
                best_metric = g_routes[i].metric;
            }
        }
    }

    if (best_match < 0) {
        spinlock_unlock(&g_route_lock);
        return -1; /* No route to host */
    }

    route_entry_t *r = &g_routes[best_match];
    if (r->flags & RT_FLAG_GATEWAY) {
        memcpy(next_hop_out, r->gateway, 4);
    } else {
        memcpy(next_hop_out, dst_ip, 4); /* Direct delivery on local segment */
    }

    *dev_out = r->dev ? r->dev : net_get_default_device();
    spinlock_unlock(&g_route_lock);

    return 0;
}

void route_print_table(void)
{
    spinlock_lock(&g_route_lock);
    pr_debug("=== IPv4 Routing Table ===\n");
    pr_debug("Destination     Gateway         Genmask         Flags Metric Iface\n");
    for (int i = 0; i < MAX_ROUTES; i++) {
        if (g_routes[i].flags & RT_FLAG_UP) {
            pr_debug("%u.%u.%u.%u     %u.%u.%u.%u     %u.%u.%u.%u     %s%s   %u     %s\n",
                     g_routes[i].dst[0], g_routes[i].dst[1], g_routes[i].dst[2], g_routes[i].dst[3],
                     g_routes[i].gateway[0], g_routes[i].gateway[1], g_routes[i].gateway[2], g_routes[i].gateway[3],
                     g_routes[i].mask[0], g_routes[i].mask[1], g_routes[i].mask[2], g_routes[i].mask[3],
                     (g_routes[i].flags & RT_FLAG_UP) ? "U" : "",
                     (g_routes[i].flags & RT_FLAG_GATEWAY) ? "G" : "",
                     g_routes[i].metric,
                     g_routes[i].dev ? g_routes[i].dev->name : "net0");
        }
    }
    spinlock_unlock(&g_route_lock);
}

/* ── IPv4 Transmission & Ingestion ────────────────────────────────────────── */

void ipv4_init(void)
{
    route_init();
    icmp_init();
    pr_debug("[IPv4] Layer 3 IPv4 engine initialized.\n");
}

int ipv4_send(net_buf_t *buf, const u8 dst_ip[4], u8 protocol)
{
    if (!buf || !dst_ip) return -1;

    u8 host_ip[4];
    net_get_ip(host_ip);

    bool is_loopback = (dst_ip[0] == 127) || (host_ip[0] != 0 && memcmp(dst_ip, host_ip, 4) == 0);

    /* 1. Prepend IPv4 Header */
    ipv4_hdr_t *ip = (ipv4_hdr_t *)net_buf_push(buf, sizeof(ipv4_hdr_t));
    if (!ip) {
        net_buf_free(buf);
        return -1;
    }

    ip->ihl_version = 0x45; /* IPv4, 20-byte header (IHL 5) */
    ip->tos = 0;
    ip->total_len = htons((u16)buf->len);
    ip->id = htons(g_ip_id_counter++);
    ip->frag_offset = 0;
    ip->ttl = 64;
    ip->protocol = protocol;
    ip->checksum = 0;
    if (dst_ip[0] == 127) {
        static const u8 loop_src[4] = { 127, 0, 0, 1 };
        memcpy(ip->src_ip, loop_src, 4);
    } else {
        memcpy(ip->src_ip, host_ip, 4);
    }
    memcpy(ip->dst_ip, dst_ip, 4);
    ip->checksum = net_checksum(ip, sizeof(ipv4_hdr_t));

    /* 2. Direct Loopback Bypass (No Ethernet / ARP needed) */
    if (is_loopback) {
        net_loopback_input(buf);
        return 0;
    }

    u8 next_hop[4];
    struct net_device *dev = NULL;
    int is_bcast = (dst_ip[0] == 255 && dst_ip[1] == 255 && dst_ip[2] == 255 && dst_ip[3] == 255);

    if (is_bcast) {
        dev = net_get_default_device();
        if (!dev) {
            net_buf_free(buf);
            return -1;
        }
    } else if (route_lookup(dst_ip, next_hop, &dev) < 0 || !dev) {
        pr_debug("[IPv4] No route to %u.%u.%u.%u\n", dst_ip[0], dst_ip[1], dst_ip[2], dst_ip[3]);
        net_buf_free(buf);
        return -1;
    }

    /* 3. Prepend Ethernet Header */
    eth_hdr_t *eth = (eth_hdr_t *)net_buf_push(buf, sizeof(eth_hdr_t));
    if (!eth) {
        net_buf_free(buf);
        return -1;
    }

    memcpy(eth->src, dev->mac, 6);
    eth->ethertype = htons(ETH_P_IP);

    /* 4. Resolve Next-Hop MAC via Broadcast or ARP */
    if (is_bcast) {
        memset(eth->dst, 0xFF, 6);
        dev->send(buf->data, buf->len);
        net_buf_free(buf);
        return 0;
    }

    u8 dst_mac[6];
    int res = arp_resolve(next_hop, dst_mac, buf);
    if (res == 0) {
        /* Immediate transmission */
        memcpy(eth->dst, dst_mac, 6);
        dev->send(buf->data, buf->len);
        net_buf_free(buf);
        return 0;
    }

    /* If res < 0, packet was queued inside ARP resolution table */
    return 0;
}

/* Weak hooks for transport layer handlers */
__attribute__((weak)) void udp_input(net_buf_t *buf, const ipv4_hdr_t *ip_hdr) { (void)buf; (void)ip_hdr; }
__attribute__((weak)) void tcp_input(net_buf_t *buf, const ipv4_hdr_t *ip_hdr) { (void)buf; (void)ip_hdr; }
__attribute__((weak)) void raw_input(net_buf_t *buf, const ipv4_hdr_t *ip_hdr) { (void)buf; (void)ip_hdr; }

void ipv4_input(net_buf_t *buf)
{
    if (!buf || buf->len < sizeof(ipv4_hdr_t)) {
        if (buf) net_buf_free(buf);
        return;
    }

    const ipv4_hdr_t *ip = (const ipv4_hdr_t *)buf->data;

    /* Validate IPv4 header */
    if ((ip->ihl_version >> 4) != 4) {
        net_buf_free(buf);
        return;
    }

    size_t ihl_bytes = (size_t)(ip->ihl_version & 0x0F) * 4;
    if (ihl_bytes < sizeof(ipv4_hdr_t) || buf->len < ihl_bytes) {
        net_buf_free(buf);
        return;
    }

    /* Verify IP checksum */
    if (net_checksum(ip, ihl_bytes) != 0) {
        pr_debug("[IPv4] Bad checksum in incoming IP packet, dropping.\n");
        net_buf_free(buf);
        return;
    }

    u8 host_ip[4];
    net_get_ip(host_ip);

    /* Accept packets for our IP, loopback 127.x.x.x, broadcast, or during DHCP setup */
    bool is_unconfigured = (host_ip[0] == 0 && host_ip[1] == 0 && host_ip[2] == 0 && host_ip[3] == 0);
    bool is_loop = (ip->dst_ip[0] == 127);
    bool is_bcast = (ip->dst_ip[0] == 255 && ip->dst_ip[1] == 255 && ip->dst_ip[2] == 255 && ip->dst_ip[3] == 255) || (ip->dst_ip[3] == 255);
    bool is_for_us = (memcmp(ip->dst_ip, host_ip, 4) == 0);

    if (!is_unconfigured && !is_loop && !is_bcast && !is_for_us) {
        net_buf_free(buf);
        return;
    }

    /* Strip IPv4 header from buffer payload */
    ipv4_hdr_t ip_copy;
    memcpy(&ip_copy, ip, sizeof(ipv4_hdr_t));
    net_buf_pull(buf, ihl_bytes);

    /* Dispatch copy to RAW sockets */
    raw_input(buf, &ip_copy);

    /* Dispatch to Layer 4 transport protocol handlers */
    if (ip_copy.protocol == IP_PROTO_ICMP) {
        icmp_input(buf, &ip_copy);
    } else if (ip_copy.protocol == IP_PROTO_UDP) {
        udp_input(buf, &ip_copy);
    } else if (ip_copy.protocol == IP_PROTO_TCP) {
        tcp_input(buf, &ip_copy);
    } else {
        /* Protocol Unreachable */
        icmp_send_dest_unreach(&ip_copy, buf->data, ICMP_CODE_PROTO_UNREACH);
        net_buf_free(buf);
    }
}
