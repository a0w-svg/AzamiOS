/* ============================================================================
 * AzamiOS — Dynamic Host Configuration Protocol Engine (dhcp.c)
 * File: kernel/net/dhcp.c
 *
 * Implements RFC 2131 / RFC 2132 DHCP 4-way client state machine (DISCOVER,
 * OFFER, REQUEST, ACK), options parsing, and automatic network lease binding.
 * ============================================================================ */

#define DEBUG 1
#include "../../include/azami/debug.h"
#include "../../include/azami/defs.h"
#include "../../include/azami/net.h"
#include "../../include/azami/net_buf.h"
#include "../../include/azami/ipv4.h"
#include "../../include/azami/udp.h"
#include "../../include/azami/dhcp.h"
#include "../../kernel/mm/kmalloc.h"
#include "../../kernel/lib/string.h"
#include "../../arch/x86_64/cpu/spinlock.h"

static dhcp_lease_t g_dhcp_lease;
static spinlock_t   g_dhcp_lock = SPINLOCK_INIT;

static inline u16 htons(u16 v) { return (u16)((v << 8) | (v >> 8)); }
static inline u16 ntohs(u16 v) { return htons(v); }
static inline u32 htonl(u32 v) { return (((v & 0xFF) << 24) | ((v & 0xFF00) << 8) | ((v & 0xFF0000) >> 8) | ((v >> 24) & 0xFF)); }
static inline u32 ntohl(u32 v) { return htonl(v); }

static void send_dhcp_udp(const dhcp_packet_t *pkt, size_t total_len)
{
    net_buf_t *buf = net_buf_alloc(NET_BUF_HEADROOM + sizeof(udp_hdr_t) + total_len);
    if (!buf) return;

    net_buf_reserve(buf, NET_BUF_HEADROOM);

    /* 1. Put UDP header */
    udp_hdr_t *udp = (udp_hdr_t *)net_buf_put(buf, sizeof(udp_hdr_t));
    udp->src_port = htons(DHCP_CLIENT_PORT);
    udp->dst_port = htons(DHCP_SERVER_PORT);
    udp->length = htons((u16)(sizeof(udp_hdr_t) + total_len));
    udp->checksum = 0;

    /* 2. Put DHCP Payload */
    void *payload = net_buf_put(buf, total_len);
    memcpy(payload, pkt, total_len);

    /* 3. Compute Checksum */
    ipv4_hdr_t pseudo_ip;
    memset(pseudo_ip.src_ip, 0, 4);
    memset(pseudo_ip.dst_ip, 0xFF, 4);
    udp->checksum = udp_checksum(udp, &pseudo_ip, payload, total_len);

    /* 4. Transmit via IPv4 broadcast */
    static const u8 bcast_ip[4] = { 255, 255, 255, 255 };
    ipv4_send(buf, bcast_ip, IP_PROTO_UDP);
}

void dhcp_init(void)
{
    spinlock_lock(&g_dhcp_lock);
    memset(&g_dhcp_lease, 0, sizeof(g_dhcp_lease));
    g_dhcp_lease.state = DHCP_STATE_INIT;
    g_dhcp_lease.xid = 0x55AA3412;
    spinlock_unlock(&g_dhcp_lock);

    pr_debug("[DHCP] RFC 2131 DHCP client subsystem initialized.\n");
}

int dhcp_start_discovery(void)
{
    spinlock_lock(&g_dhcp_lock);

    u8 host_mac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
    net_device_t *dev = net_get_default_device();
    if (dev) memcpy(host_mac, dev->mac, 6);

    dhcp_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));

    pkt.op = DHCP_BOOTREQUEST;
    pkt.htype = DHCP_HTYPE_ETH;
    pkt.hlen = 6;
    pkt.hops = 0;
    pkt.xid = htonl(g_dhcp_lease.xid);
    pkt.secs = 0;
    pkt.flags = htons(0x8000); /* Broadcast */
    memcpy(pkt.chaddr, host_mac, 6);
    pkt.magic_cookie = htonl(DHCP_MAGIC_COOKIE);

    /* Construct Options */
    u8 *opt = pkt.options;

    /* Option 53: DHCPDISCOVER */
    *opt++ = DHCP_OPT_MSG_TYPE;
    *opt++ = 1;
    *opt++ = DHCPDISCOVER;

    /* Option 55: Parameter Request List */
    *opt++ = DHCP_OPT_PARAM_REQ_LIST;
    *opt++ = 4;
    *opt++ = DHCP_OPT_SUBNET_MASK;
    *opt++ = DHCP_OPT_ROUTER;
    *opt++ = DHCP_OPT_DNS;
    *opt++ = DHCP_OPT_DOMAIN_NAME;

    /* Option 61: Client Identifier */
    *opt++ = DHCP_OPT_CLIENT_ID;
    *opt++ = 7;
    *opt++ = 1; /* Hardware Type: Ethernet */
    memcpy(opt, host_mac, 6);
    opt += 6;

    /* Option 255: END */
    *opt++ = DHCP_OPT_END;

    size_t opt_len = (size_t)(opt - pkt.options);
    size_t total_len = sizeof(dhcp_packet_t) - sizeof(pkt.options) + opt_len;

    g_dhcp_lease.state = DHCP_STATE_SELECTING;
    spinlock_unlock(&g_dhcp_lock);

    pr_debug("[DHCP] Broadcasting DHCPDISCOVER (xid: 0x%08x)...\n", g_dhcp_lease.xid);
    send_dhcp_udp(&pkt, total_len);

    /* Poll network to receive DHCPOFFER and complete DHCPACK handshake */
    for (int i = 0; i < 300; i++) {
        extern void net_poll(void);
        net_poll();
        spinlock_lock(&g_dhcp_lock);
        if (g_dhcp_lease.state == DHCP_STATE_BOUND) {
            spinlock_unlock(&g_dhcp_lock);
            break;
        }
        spinlock_unlock(&g_dhcp_lock);
        for (volatile int d = 0; d < 30000; d++) cpu_pause();
    }

    return 0;
}

static void dhcp_send_request(const dhcp_lease_t *lease)
{
    u8 host_mac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
    net_device_t *dev = net_get_default_device();
    if (dev) memcpy(host_mac, dev->mac, 6);

    dhcp_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));

    pkt.op = DHCP_BOOTREQUEST;
    pkt.htype = DHCP_HTYPE_ETH;
    pkt.hlen = 6;
    pkt.hops = 0;
    pkt.xid = htonl(lease->xid);
    pkt.secs = 0;
    pkt.flags = htons(0x8000); /* Broadcast */
    memcpy(pkt.chaddr, host_mac, 6);
    pkt.magic_cookie = htonl(DHCP_MAGIC_COOKIE);

    u8 *opt = pkt.options;

    /* Option 53: DHCPREQUEST */
    *opt++ = DHCP_OPT_MSG_TYPE;
    *opt++ = 1;
    *opt++ = DHCPREQUEST;

    /* Option 50: Requested IP */
    *opt++ = DHCP_OPT_REQUESTED_IP;
    *opt++ = 4;
    memcpy(opt, lease->offered_ip, 4);
    opt += 4;

    /* Option 54: Server Identifier */
    *opt++ = DHCP_OPT_SERVER_ID;
    *opt++ = 4;
    memcpy(opt, lease->server_id, 4);
    opt += 4;

    /* Option 55: Parameter Request List */
    *opt++ = DHCP_OPT_PARAM_REQ_LIST;
    *opt++ = 4;
    *opt++ = DHCP_OPT_SUBNET_MASK;
    *opt++ = DHCP_OPT_ROUTER;
    *opt++ = DHCP_OPT_DNS;
    *opt++ = DHCP_OPT_DOMAIN_NAME;

    /* Option 61: Client Identifier */
    *opt++ = DHCP_OPT_CLIENT_ID;
    *opt++ = 7;
    *opt++ = 1;
    memcpy(opt, host_mac, 6);
    opt += 6;

    /* Option 255: END */
    *opt++ = DHCP_OPT_END;

    size_t opt_len = (size_t)(opt - pkt.options);
    size_t total_len = sizeof(dhcp_packet_t) - sizeof(pkt.options) + opt_len;

    pr_debug("[DHCP] Sending DHCPREQUEST for %u.%u.%u.%u to server %u.%u.%u.%u...\n",
             lease->offered_ip[0], lease->offered_ip[1], lease->offered_ip[2], lease->offered_ip[3],
             lease->server_id[0], lease->server_id[1], lease->server_id[2], lease->server_id[3]);

    send_dhcp_udp(&pkt, total_len);
}

void dhcp_input(net_buf_t *buf, const ipv4_hdr_t *ip_hdr)
{
    (void)ip_hdr;
    if (!buf || buf->len < sizeof(dhcp_packet_t) - sizeof(((dhcp_packet_t *)0)->options)) {
        if (buf) net_buf_free(buf);
        return;
    }

    const dhcp_packet_t *pkt = (const dhcp_packet_t *)buf->data;

    /* Verify Magic Cookie */
    if (ntohl(pkt->magic_cookie) != DHCP_MAGIC_COOKIE) {
        net_buf_free(buf);
        return;
    }

    /* Verify Transaction ID */
    spinlock_lock(&g_dhcp_lock);
    if (ntohl(pkt->xid) != g_dhcp_lease.xid) {
        spinlock_unlock(&g_dhcp_lock);
        net_buf_free(buf);
        return;
    }

    /* Parse Options */
    const u8 *opt = pkt->options;
    const u8 *end = buf->data + buf->len;
    u8 msg_type = 0;

    u8 opt_subnet[4] = { 0, 0, 0, 0 };
    u8 opt_router[4] = { 0, 0, 0, 0 };
    u8 opt_dns[4] = { 0, 0, 0, 0 };
    u8 opt_server_id[4] = { 0, 0, 0, 0 };
    u32 opt_lease = 0;

    while (opt < end && *opt != DHCP_OPT_END) {
        if (*opt == DHCP_OPT_PAD) {
            opt++;
            continue;
        }

        u8 code = *opt++;
        if (opt >= end) break;
        u8 len = *opt++;
        if (opt + len > end) break;

        switch (code) {
        case DHCP_OPT_MSG_TYPE:
            if (len >= 1) msg_type = *opt;
            break;
        case DHCP_OPT_SUBNET_MASK:
            if (len >= 4) memcpy(opt_subnet, opt, 4);
            break;
        case DHCP_OPT_ROUTER:
            if (len >= 4) memcpy(opt_router, opt, 4);
            break;
        case DHCP_OPT_DNS:
            if (len >= 4) memcpy(opt_dns, opt, 4);
            break;
        case DHCP_OPT_SERVER_ID:
            if (len >= 4) memcpy(opt_server_id, opt, 4);
            break;
        case DHCP_OPT_LEASE_TIME:
            if (len >= 4) {
                u32 lt;
                memcpy(&lt, opt, 4);
                opt_lease = ntohl(lt);
            }
            break;
        default:
            break;
        }

        opt += len;
    }

    if (msg_type == DHCPOFFER && g_dhcp_lease.state == DHCP_STATE_SELECTING) {
        memcpy(g_dhcp_lease.offered_ip, pkt->yiaddr, 4);
        memcpy(g_dhcp_lease.netmask, opt_subnet, 4);
        memcpy(g_dhcp_lease.gateway, opt_router, 4);
        memcpy(g_dhcp_lease.dns, opt_dns, 4);
        memcpy(g_dhcp_lease.server_id, opt_server_id, 4);
        g_dhcp_lease.lease_time = opt_lease;
        g_dhcp_lease.state = DHCP_STATE_REQUESTING;

        pr_debug("[DHCP] Received DHCPOFFER of IP %u.%u.%u.%u from server %u.%u.%u.%u\n",
                 pkt->yiaddr[0], pkt->yiaddr[1], pkt->yiaddr[2], pkt->yiaddr[3],
                 opt_server_id[0], opt_server_id[1], opt_server_id[2], opt_server_id[3]);

        dhcp_send_request(&g_dhcp_lease);
    } else if (msg_type == DHCPACK && g_dhcp_lease.state == DHCP_STATE_REQUESTING) {
        memcpy(g_dhcp_lease.offered_ip, pkt->yiaddr, 4);
        g_dhcp_lease.state = DHCP_STATE_BOUND;

        /* Apply Network Configuration to Kernel */
        net_set_ip(g_dhcp_lease.offered_ip);
        net_set_netmask(g_dhcp_lease.netmask);
        net_set_gateway(g_dhcp_lease.gateway);
        net_set_dns(g_dhcp_lease.dns);

        /* Update local subnet route */
        u8 net_addr[4] = {
            (u8)(g_dhcp_lease.offered_ip[0] & g_dhcp_lease.netmask[0]),
            (u8)(g_dhcp_lease.offered_ip[1] & g_dhcp_lease.netmask[1]),
            (u8)(g_dhcp_lease.offered_ip[2] & g_dhcp_lease.netmask[2]),
            (u8)(g_dhcp_lease.offered_ip[3] & g_dhcp_lease.netmask[3])
        };
        net_device_t *net_dev = net_get_default_device();
        route_add(net_addr, g_dhcp_lease.netmask, NULL, net_dev, RT_FLAG_UP, 0);

        /* Update default gateway route */
        static const u8 default_dest[4] = { 0, 0, 0, 0 };
        static const u8 default_mask[4] = { 0, 0, 0, 0 };
        route_add(default_dest, default_mask, g_dhcp_lease.gateway, net_dev, RT_FLAG_UP | RT_FLAG_GATEWAY, 10);

        pr_debug("[DHCP] Lease BOUND successfully!\n");
        pr_debug("       IP Address:  %u.%u.%u.%u\n", g_dhcp_lease.offered_ip[0], g_dhcp_lease.offered_ip[1], g_dhcp_lease.offered_ip[2], g_dhcp_lease.offered_ip[3]);
        pr_debug("       Subnet Mask: %u.%u.%u.%u\n", g_dhcp_lease.netmask[0], g_dhcp_lease.netmask[1], g_dhcp_lease.netmask[2], g_dhcp_lease.netmask[3]);
        pr_debug("       Gateway:     %u.%u.%u.%u\n", g_dhcp_lease.gateway[0], g_dhcp_lease.gateway[1], g_dhcp_lease.gateway[2], g_dhcp_lease.gateway[3]);
        pr_debug("       DNS Server:  %u.%u.%u.%u\n", g_dhcp_lease.dns[0], g_dhcp_lease.dns[1], g_dhcp_lease.dns[2], g_dhcp_lease.dns[3]);
        pr_debug("       Lease Time:  %u seconds\n", g_dhcp_lease.lease_time);
    }

    spinlock_unlock(&g_dhcp_lock);
    net_buf_free(buf);
}

void dhcp_get_lease(dhcp_lease_t *out_lease)
{
    if (!out_lease) return;
    spinlock_lock(&g_dhcp_lock);
    memcpy(out_lease, &g_dhcp_lease, sizeof(dhcp_lease_t));
    spinlock_unlock(&g_dhcp_lock);
}
