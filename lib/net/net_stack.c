/**
 * lib/net/net_stack.c  –  AzamiOS TCP/IP Network Protocol Stack
 *
 * Kernel-independent implementation of:
 *   Ethernet II framing, ARP caching, IPv4, ICMP ping, TCP web server.
 *
 * Hardware interactions are isolated through net_hal_t (g_net_hal):
 *   - g_net_hal->send()       replaces rtl8139_send_packet()
 *   - g_net_hal->get_mac()    replaces rtl8139_dev_t.mac
 *   - g_net_hal->is_enabled() replaces rtl8139_is_enabled()
 *   - g_net_hal->log()        replaces kprintf()
 *
 * Compiles with: i686-elf-gcc -ffreestanding  OR  host gcc for testing.
 */
#include "net_stack.h"
#include "net_hal.h"
#include "../string/string.h"

/* Global HAL handle — kernel sets this before calling net_stack_init() */
net_hal_t *g_net_hal = (void*)0;

static net_config_t g_net_cfg;
static arp_entry_t  g_arp_table[16];

/* ── Logging helper ──────────────────────────────────────────────── */
#define NET_LOG(...) \
    do { if (g_net_hal && g_net_hal->log) g_net_hal->log(__VA_ARGS__); } while(0)

/* ── Checksum ────────────────────────────────────────────────────── */
static uint16_t net_checksum(const uint8_t *data, uint32_t len) {
    uint32_t sum = 0;
    const uint16_t *ptr = (const uint16_t *)data;
    while (len > 1) { sum += *ptr++; len -= 2; }
    if (len > 0)     sum += *(const uint8_t *)ptr;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum);
}

/* ── ARP Cache ───────────────────────────────────────────────────── */
static void arp_update_cache(uint32_t ip, const uint8_t *mac) {
    for (int i = 0; i < 16; i++) {
        if (g_arp_table[i].valid && g_arp_table[i].ip == ip) {
            memcpy(g_arp_table[i].mac, mac, 6);
            return;
        }
    }
    for (int i = 0; i < 16; i++) {
        if (!g_arp_table[i].valid) {
            g_arp_table[i].ip = ip;
            memcpy(g_arp_table[i].mac, mac, 6);
            g_arp_table[i].valid = true;
            return;
        }
    }
    /* Evict slot 0 */
    g_arp_table[0].ip = ip;
    memcpy(g_arp_table[0].mac, mac, 6);
    g_arp_table[0].valid = true;
}

static bool arp_lookup(uint32_t ip, uint8_t *out_mac) {
    for (int i = 0; i < 16; i++) {
        if (g_arp_table[i].valid && g_arp_table[i].ip == ip) {
            memcpy(out_mac, g_arp_table[i].mac, 6);
            return true;
        }
    }
    return false;
}

/* ── Ethernet Transmission ───────────────────────────────────────── */
static void eth_send(const uint8_t *dst_mac, uint16_t ethertype,
                     const uint8_t *payload, uint32_t len) {
    if (!g_net_hal || !g_net_hal->send) return;

    uint8_t frame[1536];
    if (len + sizeof(eth_hdr_t) > 1536) return;

    eth_hdr_t *hdr = (eth_hdr_t*)frame;
    memcpy(hdr->dst_mac, dst_mac, 6);
    memcpy(hdr->src_mac, g_net_cfg.mac_addr, 6);
    hdr->ethertype = htons(ethertype);
    memcpy(frame + sizeof(eth_hdr_t), payload, len);

    g_net_hal->send(frame, (uint32_t)(len + sizeof(eth_hdr_t)));
}

/* ── ARP ─────────────────────────────────────────────────────────── */
static void arp_send_reply(const uint8_t *target_mac, uint32_t target_ip) {
    arp_hdr_t reply;
    reply.htype  = htons(1);
    reply.ptype  = htons(ETHERTYPE_IPV4);
    reply.hlen   = 6;
    reply.plen   = 4;
    reply.opcode = htons(2);
    memcpy(reply.src_mac, g_net_cfg.mac_addr, 6);
    reply.src_ip = htonl(g_net_cfg.ip_addr);
    memcpy(reply.dst_mac, target_mac, 6);
    reply.dst_ip = target_ip;
    eth_send(target_mac, ETHERTYPE_ARP, (const uint8_t*)&reply, sizeof(arp_hdr_t));
}

static void arp_receive(const uint8_t *data, uint32_t len) {
    if (len < sizeof(arp_hdr_t)) return;
    const arp_hdr_t *arp = (const arp_hdr_t*)data;

    uint32_t sender_ip = ntohl(arp->src_ip);
    uint32_t target_ip = ntohl(arp->dst_ip);
    arp_update_cache(sender_ip, arp->src_mac);

    if (ntohs(arp->opcode) == 1 && target_ip == g_net_cfg.ip_addr)
        arp_send_reply(arp->src_mac, arp->src_ip);
}

/* ── IPv4 ────────────────────────────────────────────────────────── */
static void ipv4_send(uint32_t dst_ip, uint8_t proto,
                      const uint8_t *payload, uint32_t len) {
    uint8_t pkt[1500];
    if (len + sizeof(ipv4_hdr_t) > 1500) return;

    ipv4_hdr_t *ip = (ipv4_hdr_t*)pkt;
    ip->ver_ihl    = 0x45;
    ip->tos        = 0;
    ip->total_len  = htons((uint16_t)(len + sizeof(ipv4_hdr_t)));
    ip->id         = htons(0x1337);
    ip->flags_frag = 0;
    ip->ttl        = 64;
    ip->proto      = proto;
    ip->checksum   = 0;
    ip->src_ip     = htonl(g_net_cfg.ip_addr);
    ip->dst_ip     = htonl(dst_ip);
    ip->checksum   = net_checksum(pkt, sizeof(ipv4_hdr_t));
    memcpy(pkt + sizeof(ipv4_hdr_t), payload, len);

    uint8_t next_hop_mac[6];
    uint32_t route_ip = ((dst_ip & g_net_cfg.subnet_mask) ==
                         (g_net_cfg.ip_addr & g_net_cfg.subnet_mask))
                        ? dst_ip : g_net_cfg.gateway_ip;

    if (arp_lookup(route_ip, next_hop_mac)) {
        eth_send(next_hop_mac, ETHERTYPE_IPV4, pkt, len + sizeof(ipv4_hdr_t));
    } else {
        uint8_t bcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
        eth_send(bcast, ETHERTYPE_IPV4, pkt, len + sizeof(ipv4_hdr_t));
    }
}

/* ── ICMP ────────────────────────────────────────────────────────── */
static void icmp_receive(uint32_t src_ip, const uint8_t *data, uint32_t len) {
    if (len < sizeof(icmp_hdr_t)) return;
    const icmp_hdr_t *icmp = (const icmp_hdr_t*)data;

    if (icmp->type == 8) { /* Echo Request → Reply */
        uint8_t reply[512];
        if (len > 512) len = 512;
        memcpy(reply, data, len);
        icmp_hdr_t *rep = (icmp_hdr_t*)reply;
        rep->type = 0;
        rep->checksum = 0;
        rep->checksum = net_checksum(reply, len);
        ipv4_send(src_ip, IP_PROTO_ICMP, reply, len);
    } else if (icmp->type == 0) { /* Echo Reply */
        NET_LOG("[PING] Reply from %d.%d.%d.%d: bytes=%d ttl=64 time<1ms\n",
                (src_ip >> 24) & 0xFF, (src_ip >> 16) & 0xFF,
                (src_ip >>  8) & 0xFF,  src_ip         & 0xFF, len);
    }
}

/* ── TCP ─────────────────────────────────────────────────────────── */
static void tcp_send_packet(uint32_t dst_ip, uint16_t src_port,
                             uint16_t dst_port, uint32_t seq, uint32_t ack,
                             uint8_t flags, const uint8_t *payload,
                             uint32_t payload_len) {
    uint8_t tcp_buf[1024];
    if (payload_len + sizeof(tcp_hdr_t) > 1024) return;

    tcp_hdr_t *tcp = (tcp_hdr_t*)tcp_buf;
    tcp->src_port        = htons(src_port);
    tcp->dst_port        = htons(dst_port);
    tcp->seq_num         = htonl(seq);
    tcp->ack_num         = htonl(ack);
    tcp->data_offset_res = 0x50;
    tcp->flags           = flags;
    tcp->window_size     = htons(8192);
    tcp->checksum        = 0;
    tcp->urgent_ptr      = 0;

    struct {
        uint32_t src_ip, dst_ip;
        uint8_t  zero, proto;
        uint16_t tcp_len;
    } __attribute__((packed)) pseudo;

    pseudo.src_ip  = htonl(g_net_cfg.ip_addr);
    pseudo.dst_ip  = htonl(dst_ip);
    pseudo.zero    = 0;
    pseudo.proto   = IP_PROTO_TCP;
    pseudo.tcp_len = htons((uint16_t)(sizeof(tcp_hdr_t) + payload_len));

    if (payload_len > 0 && payload)
        memcpy(tcp_buf + sizeof(tcp_hdr_t), payload, payload_len);

    uint32_t sum = 0;
    const uint16_t *ptr = (const uint16_t *)&pseudo;
    for (uint32_t i = 0; i < sizeof(pseudo) / 2; i++) sum += *ptr++;
    ptr = (const uint16_t *)tcp_buf;
    uint32_t total_len = sizeof(tcp_hdr_t) + payload_len;
    while (total_len > 1) { sum += *ptr++; total_len -= 2; }
    if (total_len > 0) sum += *(const uint8_t *)ptr;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    tcp->checksum = (uint16_t)(~sum);

    ipv4_send(dst_ip, IP_PROTO_TCP, tcp_buf, sizeof(tcp_hdr_t) + payload_len);
}

static void tcp_receive(uint32_t src_ip, const uint8_t *data, uint32_t len) {
    if (len < sizeof(tcp_hdr_t)) return;
    const tcp_hdr_t *tcp = (const tcp_hdr_t*)data;
    uint16_t dst_port = ntohs(tcp->dst_port);
    uint16_t src_port = ntohs(tcp->src_port);
    uint32_t seq      = ntohl(tcp->seq_num);
    uint32_t ack      = ntohl(tcp->ack_num);
    uint8_t  flags    = tcp->flags;

    if (dst_port == 80) {
        if (flags & TCP_FLAG_SYN) {
            NET_LOG("[TCP_HTTP] Client %d.%d.%d.%d connected\n",
                    (src_ip>>24)&0xFF,(src_ip>>16)&0xFF,
                    (src_ip>>8) &0xFF, src_ip     &0xFF);
            tcp_send_packet(src_ip, 80, src_port, 1000, seq + 1,
                            TCP_FLAG_SYN | TCP_FLAG_ACK, (void*)0, 0);
        } else if (flags & (TCP_FLAG_PSH | TCP_FLAG_ACK)) {
            uint32_t hdr_len = (tcp->data_offset_res >> 4) * 4;
            if (len > hdr_len) {
                const char *http_resp =
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: text/plain\r\n"
                    "Connection: close\r\n\r\n"
                    "Welcome to AzamiOS Ring 0 Embedded TCP/IP Web Server!\r\n";
                uint32_t resp_len = (uint32_t)strlen(http_resp);
                uint32_t client_len = len - hdr_len;
                tcp_send_packet(src_ip, 80, src_port, ack, seq + client_len,
                                TCP_FLAG_ACK | TCP_FLAG_PSH | TCP_FLAG_FIN,
                                (const uint8_t*)http_resp, resp_len);
            }
        } else if (flags & TCP_FLAG_FIN) {
            tcp_send_packet(src_ip, 80, src_port, ack, seq + 1,
                            TCP_FLAG_ACK, (void*)0, 0);
        }
    }
}

/* ── Main Packet Dispatcher ──────────────────────────────────────── */
void net_receive_packet(const uint8_t *packet, uint32_t len) {
    if (len < sizeof(eth_hdr_t)) return;
    const eth_hdr_t *eth = (const eth_hdr_t*)packet;
    uint16_t ethertype = ntohs(eth->ethertype);

    if (ethertype == ETHERTYPE_ARP) {
        arp_receive(packet + sizeof(eth_hdr_t), len - sizeof(eth_hdr_t));
    } else if (ethertype == ETHERTYPE_IPV4) {
        if (len < sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t)) return;
        const ipv4_hdr_t *ip = (const ipv4_hdr_t*)(packet + sizeof(eth_hdr_t));
        uint32_t src_ip = ntohl(ip->src_ip);
        uint32_t dst_ip = ntohl(ip->dst_ip);

        if (dst_ip != g_net_cfg.ip_addr && dst_ip != 0xFFFFFFFF) return;
        arp_update_cache(src_ip, eth->src_mac);

        uint32_t ip_hdr_len = (ip->ver_ihl & 0x0F) * 4;
        const uint8_t *payload = packet + sizeof(eth_hdr_t) + ip_hdr_len;
        uint32_t payload_len   = len - sizeof(eth_hdr_t) - ip_hdr_len;

        if (ip->proto == IP_PROTO_ICMP)
            icmp_receive(src_ip, payload, payload_len);
        else if (ip->proto == IP_PROTO_TCP)
            tcp_receive(src_ip, payload, payload_len);
    }
}

/* ── Public API ──────────────────────────────────────────────────── */
void net_stack_init(void) {
    memset(&g_net_cfg, 0, sizeof(net_config_t));
    memset(g_arp_table, 0, sizeof(g_arp_table));

    g_net_cfg.ip_addr     = (10u<<24)|(0u<<16)|(2u<<8)|15u;  /* 10.0.2.15  */
    g_net_cfg.subnet_mask = (255u<<24)|(255u<<16)|(255u<<8)|0u; /* /24 */
    g_net_cfg.gateway_ip  = (10u<<24)|(0u<<16)|(2u<<8)|2u;   /* 10.0.2.2   */

    if (g_net_hal && g_net_hal->is_enabled && g_net_hal->is_enabled())
        g_net_hal->get_mac(g_net_cfg.mac_addr);

    NET_LOG("net: TCP/IP Protocol Stack initialized\n");
    NET_LOG("     Host IP : 10.0.2.15 | Subnet : 255.255.255.0 | Gateway : 10.0.2.2\n");
    NET_LOG("     Services: ICMP Ping Responder, HTTP Web Server (Port 80)\n");
}

void net_send_ping(uint32_t target_ip) {
    icmp_hdr_t ping;
    ping.type     = 8;
    ping.code     = 0;
    ping.checksum = 0;
    ping.id       = htons(0x4242);
    ping.seq      = htons(1);
    ping.checksum = net_checksum((const uint8_t*)&ping, sizeof(icmp_hdr_t));

    NET_LOG("\n[PING] Pinging %d.%d.%d.%d with 32 bytes of ICMP data...\n",
            (target_ip>>24)&0xFF, (target_ip>>16)&0xFF,
            (target_ip>>8) &0xFF,  target_ip     &0xFF);
    ipv4_send(target_ip, IP_PROTO_ICMP,
              (const uint8_t*)&ping, sizeof(icmp_hdr_t));
}

void net_print_arp_cache(void) {
    NET_LOG("ARP Cache Table:\n");
    bool found = false;
    for (int i = 0; i < 16; i++) {
        if (g_arp_table[i].valid) {
            uint32_t ip = g_arp_table[i].ip;
            NET_LOG("  %d.%d.%d.%d  at  %02x:%02x:%02x:%02x:%02x:%02x\n",
                    (ip>>24)&0xFF,(ip>>16)&0xFF,(ip>>8)&0xFF,ip&0xFF,
                    g_arp_table[i].mac[0],g_arp_table[i].mac[1],
                    g_arp_table[i].mac[2],g_arp_table[i].mac[3],
                    g_arp_table[i].mac[4],g_arp_table[i].mac[5]);
            found = true;
        }
    }
    if (!found) NET_LOG("  (empty)\n");
}

void net_print_status(void) {
    NET_LOG("Network Interfaces:\n");
    NET_LOG("  eth0: inet 10.0.2.15 netmask 255.255.255.0 gateway 10.0.2.2\n");
    NET_LOG("        ether %02x:%02x:%02x:%02x:%02x:%02x\n",
            g_net_cfg.mac_addr[0],g_net_cfg.mac_addr[1],
            g_net_cfg.mac_addr[2],g_net_cfg.mac_addr[3],
            g_net_cfg.mac_addr[4],g_net_cfg.mac_addr[5]);
}
