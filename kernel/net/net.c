/* ============================================================================
 * AzamiOS — Kernel Network Stack (Ethernet, ARP, IPv4, ICMP)
 * File: kernel/net/net.c
 * ============================================================================ */

#define DEBUG 1
#include "../../include/azami/debug.h"
#include "../../include/azami/defs.h"
#include "../../include/azami/net.h"
#include "../../kernel/lib/string.h"

static u8 g_host_ip[4] = { 10, 0, 2, 15 }; /* Default QEMU guest IP */
static u8 g_host_netmask[4] = { 255, 255, 255, 0 };
static u8 g_host_mac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };

#define MAX_NET_DEVS 4
static net_device_t g_net_devs[MAX_NET_DEVS];
static int          g_net_dev_count = 0;
static net_device_t *g_primary_dev = NULL;

static inline u16 htons(u16 v) { return (u16)((v << 8) | (v >> 8)); }
static inline u16 ntohs(u16 v) { return htons(v); }
static inline u32 htonl(u32 v) { return (((v & 0xFF) << 24) | ((v & 0xFF00) << 8) | ((v & 0xFF0000) >> 8) | ((v >> 24) & 0xFF)); }
static inline u32 ntohl(u32 v) { return htonl(v); }

static u16 net_checksum(const void *data, size_t len)
{
    const u16 *ptr = (const u16 *)data;
    u32 sum = 0;
    while (len > 1) {
        sum += *ptr++;
        len -= 2;
    }
    if (len > 0) {
        sum += *(const u8 *)ptr;
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return (u16)(~sum);
}

int net_register_device(const net_device_t *dev)
{
    if (!dev || g_net_dev_count >= MAX_NET_DEVS) return -1;

    memcpy(&g_net_devs[g_net_dev_count], dev, sizeof(net_device_t));
    if (!g_primary_dev) {
        g_primary_dev = &g_net_devs[g_net_dev_count];
        memcpy(g_host_mac, dev->mac, 6);
    }
    g_net_dev_count++;
    return 0;
}

net_device_t *net_get_default_device(void)
{
    return g_primary_dev;
}

void net_init(void)
{
    if (g_primary_dev) {
        memcpy(g_host_mac, g_primary_dev->mac, 6);
    }
    pr_debug("[NET] Network subsystem active. Host IP: %u.%u.%u.%u, MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
             g_host_ip[0], g_host_ip[1], g_host_ip[2], g_host_ip[3],
             g_host_mac[0], g_host_mac[1], g_host_mac[2],
             g_host_mac[3], g_host_mac[4], g_host_mac[5]);
}

void net_get_ip(u8 ip_out[4])
{
    if (ip_out) memcpy(ip_out, g_host_ip, 4);
}

void net_set_ip(const u8 ip_in[4])
{
    if (ip_in) memcpy(g_host_ip, ip_in, 4);
}

void net_get_netmask(u8 mask_out[4])
{
    if (mask_out) memcpy(mask_out, g_host_netmask, 4);
}

void net_set_netmask(const u8 mask_in[4])
{
    if (mask_in) memcpy(g_host_netmask, mask_in, 4);
}

int net_ioctl(u32 cmd, u64 arg)
{
    if (!arg && cmd != SIOCGIFFLAGS) return -1;

    switch (cmd) {
    case SIOCGIFHWADDR:
        memcpy((void *)arg, g_host_mac, 6);
        return 0;
    case SIOCGIFADDR:
        memcpy((void *)arg, g_host_ip, 4);
        return 0;
    case SIOCSIFADDR:
        memcpy(g_host_ip, (const void *)arg, 4);
        return 0;
    case SIOCGIFNETMASK:
        memcpy((void *)arg, g_host_netmask, 4);
        return 0;
    case SIOCSIFNETMASK:
        memcpy(g_host_netmask, (const void *)arg, 4);
        return 0;
    case SIOCGIFFLAGS:
        return 0x1043; /* IFF_UP | IFF_BROADCAST | IFF_RUNNING | IFF_MULTICAST */
    case SIOCGIFNAME:
        if (g_primary_dev) strcpy((char *)arg, g_primary_dev->name);
        else strcpy((char *)arg, "net0");
        return 0;
    default:
        return -1;
    }
}

static s64 net_send_raw(const void *data, size_t len)
{
    if (g_primary_dev && g_primary_dev->send) {
        return g_primary_dev->send(data, len);
    }
    return -1;
}

static void handle_arp(const u8 *pkt, size_t len)
{
    if (len < sizeof(eth_hdr_t) + sizeof(arp_pkt_t)) return;
    const arp_pkt_t *arp = (const arp_pkt_t *)(pkt + sizeof(eth_hdr_t));

    if (ntohs(arp->oper) == ARP_OP_REQUEST) {
        /* Check if ARP is asking for our IP */
        if (memcmp(arp->tpa, g_host_ip, 4) == 0) {
            u8 reply_buf[sizeof(eth_hdr_t) + sizeof(arp_pkt_t)];
            eth_hdr_t *eth_rep = (eth_hdr_t *)reply_buf;
            arp_pkt_t *arp_rep = (arp_pkt_t *)(reply_buf + sizeof(eth_hdr_t));

            /* Build Ethernet header */
            memcpy(eth_rep->dst, arp->sha, 6);
            memcpy(eth_rep->src, g_host_mac, 6);
            eth_rep->ethertype = htons(ETH_P_ARP);

            /* Build ARP reply */
            arp_rep->htype = htons(1);
            arp_rep->ptype = htons(ETH_P_IP);
            arp_rep->hlen = 6;
            arp_rep->plen = 4;
            arp_rep->oper = htons(ARP_OP_REPLY);
            memcpy(arp_rep->sha, g_host_mac, 6);
            memcpy(arp_rep->spa, g_host_ip, 4);
            memcpy(arp_rep->tha, arp->sha, 6);
            memcpy(arp_rep->tpa, arp->spa, 4);

            net_send_raw(reply_buf, sizeof(reply_buf));
        }
    }
}

static void handle_icmp(const eth_hdr_t *eth, const ipv4_hdr_t *ip, const u8 *payload, size_t len)
{
    if (len < sizeof(icmp_hdr_t)) return;
    const icmp_hdr_t *icmp = (const icmp_hdr_t *)payload;

    if (icmp->type == ICMP_TYPE_ECHO_REQUEST) {
        /* Reply with ICMP Echo Reply */
        u8 reply_buf[1500];
        size_t total_reply_len = sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t) + len;
        if (total_reply_len > sizeof(reply_buf)) return;

        eth_hdr_t *eth_rep = (eth_hdr_t *)reply_buf;
        ipv4_hdr_t *ip_rep = (ipv4_hdr_t *)(reply_buf + sizeof(eth_hdr_t));
        icmp_hdr_t *icmp_rep = (icmp_hdr_t *)(reply_buf + sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t));

        /* Ethernet Header */
        memcpy(eth_rep->dst, eth->src, 6);
        memcpy(eth_rep->src, g_host_mac, 6);
        eth_rep->ethertype = htons(ETH_P_IP);

        /* IP Header */
        ip_rep->ihl_version = 0x45;
        ip_rep->tos = 0;
        ip_rep->total_len = htons((u16)(sizeof(ipv4_hdr_t) + len));
        ip_rep->id = htons(1);
        ip_rep->frag_offset = 0;
        ip_rep->ttl = 64;
        ip_rep->protocol = IP_PROTO_ICMP;
        ip_rep->checksum = 0;
        memcpy(ip_rep->src_ip, g_host_ip, 4);
        memcpy(ip_rep->dst_ip, ip->src_ip, 4);
        ip_rep->checksum = net_checksum(ip_rep, sizeof(ipv4_hdr_t));

        /* ICMP Header + Data */
        memcpy(icmp_rep, icmp, len);
        icmp_rep->type = ICMP_TYPE_ECHO_REPLY;
        icmp_rep->checksum = 0;
        icmp_rep->checksum = net_checksum(icmp_rep, len);

        net_send_raw(reply_buf, total_reply_len);
    }
}

void net_process_incoming(const u8 *pkt, size_t len)
{
    if (!pkt || len < sizeof(eth_hdr_t)) return;
    const eth_hdr_t *eth = (const eth_hdr_t *)pkt;
    u16 ethertype = ntohs(eth->ethertype);

    if (ethertype == ETH_P_ARP) {
        handle_arp(pkt, len);
    } else if (ethertype == ETH_P_IP) {
        if (len < sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t)) return;
        const ipv4_hdr_t *ip = (const ipv4_hdr_t *)(pkt + sizeof(eth_hdr_t));

        /* Check destination IP */
        if (memcmp(ip->dst_ip, g_host_ip, 4) != 0 && ip->dst_ip[0] != 255) return;

        size_t ip_hdr_len = (ip->ihl_version & 0x0F) * 4;
        const u8 *payload = pkt + sizeof(eth_hdr_t) + ip_hdr_len;
        size_t payload_len = len - sizeof(eth_hdr_t) - ip_hdr_len;

        if (ip->protocol == IP_PROTO_ICMP) {
            handle_icmp(eth, ip, payload, payload_len);
        }
    }
}

s64 net_send_icmp_ping(const u8 target_ip[4], u16 seq)
{
    u8 pkt[sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t) + sizeof(icmp_hdr_t) + 32];
    eth_hdr_t *eth = (eth_hdr_t *)pkt;
    ipv4_hdr_t *ip = (ipv4_hdr_t *)(pkt + sizeof(eth_hdr_t));
    icmp_hdr_t *icmp = (icmp_hdr_t *)(pkt + sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t));
    u8 *data = pkt + sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t) + sizeof(icmp_hdr_t);

    /* Ethernet (Broadcast or Gateway in QEMU) */
    memset(eth->dst, 0xFF, 6);
    memcpy(eth->src, g_host_mac, 6);
    eth->ethertype = htons(ETH_P_IP);

    /* IPv4 */
    ip->ihl_version = 0x45;
    ip->tos = 0;
    ip->total_len = htons((u16)(sizeof(ipv4_hdr_t) + sizeof(icmp_hdr_t) + 32));
    ip->id = htons(seq);
    ip->frag_offset = 0;
    ip->ttl = 64;
    ip->protocol = IP_PROTO_ICMP;
    ip->checksum = 0;
    memcpy(ip->src_ip, g_host_ip, 4);
    memcpy(ip->dst_ip, target_ip, 4);
    ip->checksum = net_checksum(ip, sizeof(ipv4_hdr_t));

    /* ICMP */
    icmp->type = ICMP_TYPE_ECHO_REQUEST;
    icmp->code = 0;
    icmp->checksum = 0;
    icmp->id = htons(0x1234);
    icmp->seq = htons(seq);

    for (int i = 0; i < 32; i++) data[i] = (u8)('a' + (i % 26));

    icmp->checksum = net_checksum(icmp, sizeof(icmp_hdr_t) + 32);

    return net_send_raw(pkt, sizeof(pkt));
}
