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
#include "../../include/azami/udp.h"
#include "../../include/azami/tcp.h"
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

    /*
     * The UDP and TCP checksums cover a pseudo-header built from the final
     * source and destination addresses, so they can only be computed once the
     * IP header exists — which is here.  A transport that computed one from
     * its own guess at the source address would be wrong whenever that guess
     * differed from what this function actually wrote: the DHCP client, for
     * one, always assumes 0.0.0.0, so every packet it sent after an address
     * was configured carried a checksum for the wrong pseudo-header and was
     * silently dropped by the peer.
     */
    size_t l4_len = buf->len - sizeof(ipv4_hdr_t);
    if (protocol == IP_PROTO_UDP && l4_len >= sizeof(udp_hdr_t)) {
        udp_hdr_t *udp = (udp_hdr_t *)((u8 *)ip + sizeof(ipv4_hdr_t));
        size_t udp_len = ntohs(udp->length);
        if (udp_len >= sizeof(udp_hdr_t) && udp_len <= l4_len) {
            udp->checksum = 0;
            udp->checksum = udp_checksum(udp, ip, (const u8 *)udp + sizeof(udp_hdr_t),
                                         udp_len - sizeof(udp_hdr_t));
        }
    } else if (protocol == IP_PROTO_TCP && l4_len >= sizeof(tcp_hdr_t)) {
        tcp_hdr_t *tcp = (tcp_hdr_t *)((u8 *)ip + sizeof(ipv4_hdr_t));
        /* tcp_checksum() now covers whatever header the segment declares, so
         * a segment carrying options gets a correct checksum instead of being
         * skipped and sent out with whatever was in the field. */
        size_t hdr_len = (size_t)((tcp->data_offset >> 4) * 4);
        if (hdr_len >= sizeof(tcp_hdr_t) && hdr_len <= l4_len) {
            tcp->checksum = 0;
            tcp->checksum = tcp_checksum(tcp, ip, hdr_len,
                                         (const u8 *)tcp + hdr_len,
                                         l4_len - hdr_len);
        }
    }

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

    /*
     * total_len — not buf->len — is where the datagram ends.
     *
     * Ethernet pads every frame out to a 60-byte minimum, so a received buffer
     * is routinely longer than the packet inside it.  Nothing used to trim it,
     * and the transports that size their payload from buf->len rather than
     * from their own length field inherited the padding: TCP summed it into
     * the segment checksum, so every minimum-size segment — a bare ACK, SYN,
     * FIN or RST is 54 bytes on the wire and therefore always padded — failed
     * validation and was dropped, which is most of a connection's traffic.
     */
    size_t total_len = ntohs(ip->total_len);
    if (total_len < ihl_bytes || total_len > buf->len) {
        pr_debug("[IPv4] Bad total_len %zu (ihl=%zu, have=%zu), dropping.\n",
                 total_len, ihl_bytes, buf->len);
        net_buf_free(buf);
        return;
    }
    net_buf_trim(buf, total_len);

    /*
     * Fragments are dropped rather than handed up half-parsed.  There is no
     * reassembly engine here, and a non-first fragment carries no transport
     * header at all — passing one to tcp_input()/udp_input() would have them
     * read the payload as though it were a header.
     */
    u16 frag = ntohs(ip->frag_offset);
    if ((frag & IP_FLAG_MF) || (frag & IP_FRAG_OFF_MASK)) {
        pr_debug("[IPv4] Fragmented datagram (off=%u, MF=%u) — no reassembly, dropping.\n",
                 (unsigned)(frag & IP_FRAG_OFF_MASK), (unsigned)!!(frag & IP_FLAG_MF));
        net_buf_free(buf);
        return;
    }

    u8 host_ip[4], host_mask[4];
    net_get_ip(host_ip);
    net_get_netmask(host_mask);

    /* Accept packets for our IP, loopback 127.x.x.x, broadcast, or during DHCP setup */
    bool is_unconfigured = (host_ip[0] == 0 && host_ip[1] == 0 && host_ip[2] == 0 && host_ip[3] == 0);
    bool is_loop = (ip->dst_ip[0] == 127);
    bool is_for_us = (memcmp(ip->dst_ip, host_ip, 4) == 0);

    /*
     * Broadcast is either the limited broadcast 255.255.255.255 or *our*
     * subnet's directed broadcast.  The old test accepted any address whose
     * last octet was 255, which on a /16 or wider is an ordinary unicast
     * address belonging to some other host.
     */
    bool is_bcast = (ip->dst_ip[0] == 255 && ip->dst_ip[1] == 255 &&
                     ip->dst_ip[2] == 255 && ip->dst_ip[3] == 255);
    if (!is_bcast && !is_unconfigured) {
        bool directed = true;
        for (int k = 0; k < 4; k++) {
            u8 want = (u8)((host_ip[k] & host_mask[k]) | (u8)~host_mask[k]);
            if (ip->dst_ip[k] != want) { directed = false; break; }
        }
        is_bcast = directed;
    }

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
        /* Protocol Unreachable.  buf->data is what is left after the header
         * was pulled, and for a 20-byte datagram that is nothing at all —
         * hence the explicit length: the quoted-payload copy used to read a
         * fixed 8 bytes off the end of the heap allocation. */
        icmp_send_dest_unreach(&ip_copy, buf->data, buf->len, ICMP_CODE_PROTO_UNREACH);
        net_buf_free(buf);
    }
}
