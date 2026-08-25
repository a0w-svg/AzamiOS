/* ============================================================================
 * AzamiOS — Kernel Network Stack (Ethernet, ARP, IPv4, ICMP)
 * File: kernel/net/net.c
 * ============================================================================ */

#define DEBUG 1
#include "../../include/azami/debug.h"
#include "../../include/azami/defs.h"
#include "../../include/azami/net.h"
#include "../../include/azami/arp.h"
#include "../../include/azami/net_buf.h"
#include "../../include/azami/ipv4.h"
#include "../../include/azami/icmp.h"
#include "../../kernel/uaccess.h"
#include "../../kernel/lib/string.h"

static u8 g_host_ip[4] = { 0, 0, 0, 0 };      /* Unconfigured until DHCP */
static u8 g_host_netmask[4] = { 0, 0, 0, 0 };
static u8 g_host_gateway[4] = { 0, 0, 0, 0 };
static u8 g_host_dns[4] = { 0, 0, 0, 0 };
static u8 g_host_mac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };

#define MAX_NET_DEVS 8
static net_device_t g_net_devs[MAX_NET_DEVS];
static int          g_net_dev_count = 0;
static net_device_t *g_primary_dev = NULL;
static net_device_t  g_loopback_dev;

static inline u16 htons(u16 v) { return (u16)((v << 8) | (v >> 8)); }
static inline u16 ntohs(u16 v) { return htons(v); }

static s64 loopback_send(const void *data, size_t len)
{
    if (!data || len == 0) return 0;
    net_buf_t *buf = net_buf_alloc(len + 32);
    if (!buf) return -ENOMEM;
    void *p = net_buf_put(buf, len);
    memcpy(p, data, len);
    ipv4_input(buf);
    g_loopback_dev.stats.tx_packets++;
    g_loopback_dev.stats.tx_bytes += len;
    g_loopback_dev.stats.rx_packets++;
    g_loopback_dev.stats.rx_bytes += len;
    return (s64)len;
}

u16 net_checksum(const void *data, size_t len)
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
    if (!g_primary_dev && !(dev->flags & IFF_LOOPBACK)) {
        g_primary_dev = &g_net_devs[g_net_dev_count];
        memcpy(g_host_mac, dev->mac, 6);
    }
    g_net_dev_count++;
    return 0;
}

net_device_t *net_get_default_device(void)
{
    return g_primary_dev ? g_primary_dev : &g_loopback_dev;
}

net_device_t *net_get_device_by_name(const char *name)
{
    if (!name) return NULL;
    if (strcmp(name, "lo") == 0 || strcmp(name, "lo0") == 0) return &g_loopback_dev;

    for (int i = 0; i < g_net_dev_count; i++) {
        if (strcmp(g_net_devs[i].name, name) == 0) {
            return &g_net_devs[i];
        }
    }
    return NULL;
}

net_device_t *net_get_device_by_index(int index)
{
    if (index == 0) return &g_loopback_dev;
    if (index > 0 && index <= g_net_dev_count) return &g_net_devs[index - 1];
    return NULL;
}

int net_get_device_count(void)
{
    return g_net_dev_count + 1; /* +1 for lo0 */
}

void net_loopback_input(net_buf_t *buf)
{
    if (!buf) return;
    g_loopback_dev.stats.rx_packets++;
    g_loopback_dev.stats.rx_bytes += buf->len;
    ipv4_input(buf);
}

#include "../../include/azami/udp.h"
#include "../../include/azami/tcp.h"
#include "../../include/azami/dhcp.h"

void net_init(void)
{
    /* 1. Initialize Loopback Interface (lo0) */
    memset(&g_loopback_dev, 0, sizeof(net_device_t));
    strcpy(g_loopback_dev.name, "lo0");
    g_loopback_dev.ip[0] = 127; g_loopback_dev.ip[1] = 0; g_loopback_dev.ip[2] = 0; g_loopback_dev.ip[3] = 1;
    g_loopback_dev.netmask[0] = 255; g_loopback_dev.netmask[1] = 0; g_loopback_dev.netmask[2] = 0; g_loopback_dev.netmask[3] = 0;
    g_loopback_dev.broadcast[0] = 127; g_loopback_dev.broadcast[1] = 255; g_loopback_dev.broadcast[2] = 255; g_loopback_dev.broadcast[3] = 255;
    g_loopback_dev.flags = IFF_UP | IFF_LOOPBACK | IFF_RUNNING;
    g_loopback_dev.mtu = 65536;
    g_loopback_dev.send = loopback_send;
    g_loopback_dev.link_up = NULL;

    if (g_primary_dev) {
        memcpy(g_host_mac, g_primary_dev->mac, 6);
    }

    /* Initialize dynamic ARP cache table */
    arp_init();

    /* Initialize IPv4 routing and ICMP */
    ipv4_init();

    /* Initialize UDP transport layer */
    udp_init();

    /* Initialize TCP transport layer */
    tcp_init();

    /* Initialize RFC 2131 DHCP Client and start discovery */
    dhcp_init();

    pr_debug("[NET] Network subsystem active. Loopback: 127.0.0.1, MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
             g_host_mac[0], g_host_mac[1], g_host_mac[2],
             g_host_mac[3], g_host_mac[4], g_host_mac[5]);

    /* Initiate real DHCP lease discovery over the network */
    dhcp_start_discovery();
}

void net_get_ip(u8 ip_out[4])
{
    if (ip_out) memcpy(ip_out, g_host_ip, 4);
}

void net_set_ip(const u8 ip_in[4])
{
    if (!ip_in) return;
    memcpy(g_host_ip, ip_in, 4);

    /* Update subnet route if netmask is non-zero */
    if (g_host_netmask[0] != 0) {
        u8 net_addr[4] = {
            (u8)(g_host_ip[0] & g_host_netmask[0]),
            (u8)(g_host_ip[1] & g_host_netmask[1]),
            (u8)(g_host_ip[2] & g_host_netmask[2]),
            (u8)(g_host_ip[3] & g_host_netmask[3])
        };
        route_add(net_addr, g_host_netmask, NULL, g_primary_dev, RT_FLAG_UP, 0);
    }
}

void net_get_netmask(u8 mask_out[4])
{
    if (mask_out) memcpy(mask_out, g_host_netmask, 4);
}

void net_set_netmask(const u8 mask_in[4])
{
    if (mask_in) memcpy(g_host_netmask, mask_in, 4);
}

void net_get_gateway(u8 gw_out[4])
{
    if (gw_out) memcpy(gw_out, g_host_gateway, 4);
}

void net_set_gateway(const u8 gw_in[4])
{
    if (!gw_in) return;
    memcpy(g_host_gateway, gw_in, 4);

    if (g_host_gateway[0] != 0 || g_host_gateway[1] != 0 ||
        g_host_gateway[2] != 0 || g_host_gateway[3] != 0) {
        static const u8 default_dest[4] = { 0, 0, 0, 0 };
        static const u8 default_mask[4] = { 0, 0, 0, 0 };
        route_add(default_dest, default_mask, g_host_gateway, g_primary_dev, RT_FLAG_UP | RT_FLAG_GATEWAY, 10);
    }
}

void net_get_dns(u8 dns_out[4])
{
    if (dns_out) memcpy(dns_out, g_host_dns, 4);
}

void net_set_dns(const u8 dns_in[4])
{
    if (dns_in) memcpy(g_host_dns, dns_in, 4);
}

int net_ioctl(u32 cmd, u64 arg)
{
    if (!arg && cmd != SIOCGIFFLAGS && cmd != SIOCSIFDHCP) return -1;
    if (cmd != SIOCGIFFLAGS && cmd != SIOCSIFDHCP && (uintptr_t)arg >= 0x8000000000000000ULL) return -1;

    switch (cmd) {
    case SIOCGIFHWADDR:
        if (copy_to_user((void *)(uintptr_t)arg, g_host_mac, 6) != 0) return -1;
        return 0;
    case SIOCGIFADDR:
        if (copy_to_user((void *)(uintptr_t)arg, g_host_ip, 4) != 0) return -1;
        return 0;
    case SIOCSIFADDR: {
        u8 new_ip[4];
        if (copy_from_user(new_ip, (const void *)(uintptr_t)arg, 4) != 0) return -1;
        net_set_ip(new_ip);
        return 0;
    }
    case SIOCGIFNETMASK:
        if (copy_to_user((void *)(uintptr_t)arg, g_host_netmask, 4) != 0) return -1;
        return 0;
    case SIOCSIFNETMASK: {
        u8 new_mask[4];
        if (copy_from_user(new_mask, (const void *)(uintptr_t)arg, 4) != 0) return -1;
        net_set_netmask(new_mask);
        return 0;
    }
    case SIOCGIFGW:
        if (copy_to_user((void *)(uintptr_t)arg, g_host_gateway, 4) != 0) return -1;
        return 0;
    case SIOCSIFGW: {
        u8 new_gw[4];
        if (copy_from_user(new_gw, (const void *)(uintptr_t)arg, 4) != 0) return -1;
        net_set_gateway(new_gw);
        return 0;
    }
    case SIOCGIFDNS:
        if (copy_to_user((void *)(uintptr_t)arg, g_host_dns, 4) != 0) return -1;
        return 0;
    case SIOCSIFDNS: {
        u8 new_dns[4];
        if (copy_from_user(new_dns, (const void *)(uintptr_t)arg, 4) != 0) return -1;
        net_set_dns(new_dns);
        return 0;
    }
    case SIOCGIFFLAGS:
        return (int)(g_primary_dev ? g_primary_dev->flags : (IFF_UP | IFF_BROADCAST | IFF_RUNNING | IFF_MULTICAST));
    case SIOCSIFFLAGS: {
        u32 flg = 0;
        if (copy_from_user(&flg, (const void *)(uintptr_t)arg, sizeof(u32)) == 0) {
            if (g_primary_dev) g_primary_dev->flags = flg;
            return 0;
        }
        return -1;
    }
    case SIOCGIFNAME: {
        const char *name = g_primary_dev ? g_primary_dev->name : "net0";
        size_t nlen = strlen(name) + 1;
        if (copy_to_user((void *)(uintptr_t)arg, name, nlen) != 0) return -1;
        return 0;
    }
    case SIOCGIFMTU: {
        u32 mtu = g_primary_dev ? g_primary_dev->mtu : 1500;
        if (copy_to_user((void *)(uintptr_t)arg, &mtu, sizeof(u32)) != 0) return -1;
        return 0;
    }
    case SIOCGIFBRDADDR: {
        u8 brd[4] = {
            (u8)(g_host_ip[0] | ~g_host_netmask[0]),
            (u8)(g_host_ip[1] | ~g_host_netmask[1]),
            (u8)(g_host_ip[2] | ~g_host_netmask[2]),
            (u8)(g_host_ip[3] | ~g_host_netmask[3])
        };
        if (copy_to_user((void *)(uintptr_t)arg, brd, 4) != 0) return -1;
        return 0;
    }
    case SIOCGIFSTATS: {
        net_stats_t st = g_primary_dev ? g_primary_dev->stats : g_loopback_dev.stats;
        if (copy_to_user((void *)(uintptr_t)arg, &st, sizeof(net_stats_t)) != 0) return -1;
        return 0;
    }
    case SIOCGIFCOUNT: {
        int count = net_get_device_count();
        if (copy_to_user((void *)(uintptr_t)arg, &count, sizeof(int)) != 0) return -1;
        return 0;
    }
    case SIOCGIFINDEX: {
        int idx = 0;
        if (copy_from_user(&idx, (const void *)(uintptr_t)arg, sizeof(int)) != 0) return -1;
        net_device_t *dev = net_get_device_by_index(idx);
        if (!dev) return -1;
        if (copy_to_user((void *)(uintptr_t)arg, dev, sizeof(net_device_t)) != 0) return -1;
        return 0;
    }
    case SIOCSIFDHCP: {
        extern int dhcp_trigger_renew(void);
        return dhcp_trigger_renew();
    }
    case SIOCGIFDHCP: {
        extern void dhcp_get_lease(dhcp_lease_t *out_lease);
        dhcp_lease_t lease;
        dhcp_get_lease(&lease);
        if (copy_to_user((void *)(uintptr_t)arg, &lease, sizeof(dhcp_lease_t)) != 0) return -1;
        return 0;
    }
    default:
        return -1;
    }
}

s64 net_send_raw(const void *data, size_t len)
{
    if (g_primary_dev && g_primary_dev->send) {
        return g_primary_dev->send(data, len);
    }
    return -1;
}

void net_process_incoming(const u8 *pkt, size_t len)
{
    if (!pkt || len < sizeof(eth_hdr_t)) return;
    const eth_hdr_t *eth = (const eth_hdr_t *)pkt;
    u16 ethertype = ntohs(eth->ethertype);

    if (ethertype == ETH_P_ARP) {
        arp_input(pkt, len);
    } else if (ethertype == ETH_P_IP) {
        size_t ip_len = len - sizeof(eth_hdr_t);
        net_buf_t *buf = net_buf_alloc(ip_len + 32);
        if (!buf) return;

        void *payload = net_buf_put(buf, ip_len);
        memcpy(payload, pkt + sizeof(eth_hdr_t), ip_len);
        ipv4_input(buf);
    }
}

__attribute__((weak)) void e1000_poll_rx(void) {}
__attribute__((weak)) void virtio_net_poll(void) {}

void net_poll(void)
{
    e1000_poll_rx();
    virtio_net_poll();
}

s64 net_send_icmp_ping(const u8 target_ip[4], u16 seq)
{
    return icmp_send_echo(target_ip, 0x1234, seq, NULL, 32);
}
