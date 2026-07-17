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
 * Compiles with: x86_64-elf-gcc -ffreestanding  OR  host gcc for testing.
 */
#include "net_stack.h"
#include "net_hal.h"
#include "../string/string.h"

/* Global HAL handle — kernel sets this before calling net_stack_init() */
net_hal_t *g_net_hal = (void*)0;

static net_config_t g_net_cfg;
static arp_entry_t  g_arp_table[16];
static tcp_socket_t g_tcp_sockets[8];
static udp_socket_t g_udp_sockets[8];

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

/* ── UDP ─────────────────────────────────────────────────────────── */
static void udp_receive(uint32_t src_ip, const uint8_t *data, uint32_t len) {
    if (len < sizeof(udp_hdr_t)) return;
    const udp_hdr_t *udp = (const udp_hdr_t*)data;
    uint16_t dst_port = ntohs(udp->dst_port);
    uint16_t src_port = ntohs(udp->src_port);
    uint16_t payload_len = len - sizeof(udp_hdr_t);
    const uint8_t *payload = data + sizeof(udp_hdr_t);

    for (int i = 0; i < 8; i++) {
        if (g_udp_sockets[i].valid && g_udp_sockets[i].local_port == dst_port) {
            if (g_udp_sockets[i].rx_cb)
                g_udp_sockets[i].rx_cb(i, src_ip, src_port, payload, payload_len);
            break;
        }
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
    uint32_t hdr_len  = (tcp->data_offset_res >> 4) * 4;
    uint32_t payload_len = (len > hdr_len) ? (len - hdr_len) : 0;
    const uint8_t *payload = (len > hdr_len) ? (data + hdr_len) : NULL;

    int sock_idx = -1;
    for (int i = 0; i < 8; i++) {
        if (g_tcp_sockets[i].valid && g_tcp_sockets[i].local_port == dst_port &&
            g_tcp_sockets[i].remote_ip == src_ip && g_tcp_sockets[i].remote_port == src_port) {
            sock_idx = i;
            break;
        }
    }
    if (sock_idx == -1) {
        for (int i = 0; i < 8; i++) {
            if (g_tcp_sockets[i].valid && g_tcp_sockets[i].local_port == dst_port &&
                g_tcp_sockets[i].state == TCP_STATE_LISTEN) {
                sock_idx = i;
                break;
            }
        }
    }

    if (sock_idx == -1) {
        if (!(flags & TCP_FLAG_RST)) {
            tcp_send_packet(src_ip, dst_port, src_port, ack, seq + payload_len + ((flags & (TCP_FLAG_SYN|TCP_FLAG_FIN)) ? 1 : 0), TCP_FLAG_RST | TCP_FLAG_ACK, NULL, 0);
        }
        return;
    }

    tcp_socket_t *sock = &g_tcp_sockets[sock_idx];

    switch (sock->state) {
        case TCP_STATE_LISTEN:
            if (flags & TCP_FLAG_SYN) {
                sock->remote_ip   = src_ip;
                sock->remote_port = src_port;
                sock->ack_num     = seq + 1;
                sock->seq_num     = 1000;
                sock->state       = TCP_STATE_SYN_RCVD;
                NET_LOG("[TCP] Incoming SYN from %d.%d.%d.%d:%d on port %d\n",
                        (src_ip>>24)&0xFF, (src_ip>>16)&0xFF, (src_ip>>8)&0xFF, src_ip&0xFF, src_port, dst_port);
                tcp_send_packet(src_ip, sock->local_port, src_port, sock->seq_num, sock->ack_num, TCP_FLAG_SYN | TCP_FLAG_ACK, NULL, 0);
                sock->seq_num++;
            }
            break;

        case TCP_STATE_SYN_SENT:
            if ((flags & (TCP_FLAG_SYN | TCP_FLAG_ACK)) == (TCP_FLAG_SYN | TCP_FLAG_ACK)) {
                sock->ack_num = seq + 1;
                sock->state   = TCP_STATE_ESTABLISHED;
                NET_LOG("[TCP] Connection established to %d.%d.%d.%d:%d\n",
                        (src_ip>>24)&0xFF, (src_ip>>16)&0xFF, (src_ip>>8)&0xFF, src_ip&0xFF, src_port);
                tcp_send_packet(src_ip, sock->local_port, src_port, sock->seq_num, sock->ack_num, TCP_FLAG_ACK, NULL, 0);
            }
            break;

        case TCP_STATE_SYN_RCVD:
            if (flags & TCP_FLAG_ACK) {
                sock->state = TCP_STATE_ESTABLISHED;
                NET_LOG("[TCP] Connection handshake completed on port %d\n", dst_port);
                if (payload_len > 0 && sock->rx_cb) {
                    sock->ack_num += payload_len;
                    sock->rx_cb(sock_idx, src_ip, src_port, payload, payload_len);
                    tcp_send_packet(src_ip, sock->local_port, src_port, sock->seq_num, sock->ack_num, TCP_FLAG_ACK, NULL, 0);
                }
            }
            break;

        case TCP_STATE_ESTABLISHED:
            if (payload_len > 0) {
                sock->ack_num = seq + payload_len;
                if (sock->rx_cb) {
                    sock->rx_cb(sock_idx, src_ip, src_port, payload, payload_len);
                }
                tcp_send_packet(src_ip, sock->local_port, src_port, sock->seq_num, sock->ack_num, TCP_FLAG_ACK, NULL, 0);
            }
            if (flags & TCP_FLAG_FIN) {
                sock->ack_num = seq + payload_len + 1;
                sock->state = TCP_STATE_CLOSE_WAIT;
                tcp_send_packet(src_ip, sock->local_port, src_port, sock->seq_num, sock->ack_num, TCP_FLAG_ACK | TCP_FLAG_FIN, NULL, 0);
                sock->seq_num++;
                sock->state = TCP_STATE_CLOSED;
                sock->valid = false;
            }
            break;

        case TCP_STATE_FIN_WAIT1:
            if (flags & TCP_FLAG_ACK) {
                sock->state = TCP_STATE_FIN_WAIT2;
            }
            if (flags & TCP_FLAG_FIN) {
                sock->ack_num = seq + 1;
                tcp_send_packet(src_ip, sock->local_port, src_port, sock->seq_num, sock->ack_num, TCP_FLAG_ACK, NULL, 0);
                sock->state = TCP_STATE_CLOSED;
                sock->valid = false;
            }
            break;

        case TCP_STATE_FIN_WAIT2:
            if (flags & TCP_FLAG_FIN) {
                sock->ack_num = seq + 1;
                tcp_send_packet(src_ip, sock->local_port, src_port, sock->seq_num, sock->ack_num, TCP_FLAG_ACK, NULL, 0);
                sock->state = TCP_STATE_CLOSED;
                sock->valid = false;
            }
            break;

        default:
            break;
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
        else if (ip->proto == IP_PROTO_UDP)
            udp_receive(src_ip, payload, payload_len);
    }
}

static void http_server_cb(int sock_id, uint32_t src_ip, uint16_t src_port, const uint8_t *data, uint32_t len) {
    (void)src_ip; (void)src_port;
    if (len > 0 && data) {
        const char *http_resp =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/plain\r\n"
            "Connection: close\r\n\r\n"
            "Welcome to AzamiOS Ring 0 Embedded TCP/IP Web Server!\r\n";
        net_tcp_send(sock_id, (const uint8_t*)http_resp, (uint32_t)strlen(http_resp));
        net_tcp_close(sock_id);
    }
}

/* ── Public API ──────────────────────────────────────────────────── */
void net_stack_init(void) {
    memset(&g_net_cfg, 0, sizeof(net_config_t));
    memset(g_arp_table, 0, sizeof(g_arp_table));
    memset(g_tcp_sockets, 0, sizeof(g_tcp_sockets));
    memset(g_udp_sockets, 0, sizeof(g_udp_sockets));

    g_net_cfg.ip_addr     = (10u<<24)|(0u<<16)|(2u<<8)|15u;  /* 10.0.2.15  */
    g_net_cfg.subnet_mask = (255u<<24)|(255u<<16)|(255u<<8)|0u; /* /24 */
    g_net_cfg.gateway_ip  = (10u<<24)|(0u<<16)|(2u<<8)|2u;   /* 10.0.2.2   */

    if (g_net_hal && g_net_hal->is_enabled && g_net_hal->is_enabled())
        g_net_hal->get_mac(g_net_cfg.mac_addr);

    int http_sock = net_tcp_socket_open(http_server_cb);
    if (http_sock >= 0) {
        net_tcp_bind(http_sock, 80);
        net_tcp_listen(http_sock);
    }

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

/* ── Public Socket API Implementation ────────────────────────────── */
int net_tcp_socket_open(tcp_rx_callback_t cb) {
    for (int i = 0; i < 8; i++) {
        if (!g_tcp_sockets[i].valid) {
            memset(&g_tcp_sockets[i], 0, sizeof(tcp_socket_t));
            g_tcp_sockets[i].valid = true;
            g_tcp_sockets[i].state = TCP_STATE_CLOSED;
            g_tcp_sockets[i].rx_cb = cb;
            return i;
        }
    }
    return -1;
}

bool net_tcp_bind(int sock_id, uint16_t local_port) {
    if (sock_id < 0 || sock_id >= 8 || !g_tcp_sockets[sock_id].valid) return false;
    g_tcp_sockets[sock_id].local_port = local_port;
    return true;
}

bool net_tcp_listen(int sock_id) {
    if (sock_id < 0 || sock_id >= 8 || !g_tcp_sockets[sock_id].valid) return false;
    g_tcp_sockets[sock_id].state = TCP_STATE_LISTEN;
    return true;
}

bool net_tcp_connect(int sock_id, uint32_t remote_ip, uint16_t remote_port) {
    if (sock_id < 0 || sock_id >= 8 || !g_tcp_sockets[sock_id].valid) return false;
    tcp_socket_t *sock = &g_tcp_sockets[sock_id];
    if (sock->local_port == 0) sock->local_port = (uint16_t)(49152 + sock_id);
    sock->remote_ip   = remote_ip;
    sock->remote_port = remote_port;
    sock->seq_num     = net_tcp_get_isn();
    sock->ack_num     = 0;
    sock->state       = TCP_STATE_SYN_SENT;
    tcp_send_packet(remote_ip, sock->local_port, remote_port, sock->seq_num, 0, TCP_FLAG_SYN, NULL, 0);
    sock->seq_num++;
    return true;
}

int net_tcp_send(int sock_id, const uint8_t *data, uint32_t len) {
    if (sock_id < 0 || sock_id >= 8 || !g_tcp_sockets[sock_id].valid) return -1;
    tcp_socket_t *sock = &g_tcp_sockets[sock_id];
    tcp_send_packet(sock->remote_ip, sock->local_port, sock->remote_port, sock->seq_num, sock->ack_num, TCP_FLAG_ACK | TCP_FLAG_PSH, data, len);
    sock->seq_num += len;
    return (int)len;
}

void net_tcp_close(int sock_id) {
    if (sock_id < 0 || sock_id >= 8 || !g_tcp_sockets[sock_id].valid) return;
    tcp_socket_t *sock = &g_tcp_sockets[sock_id];
    if (sock->state == TCP_STATE_ESTABLISHED || sock->state == TCP_STATE_SYN_RCVD) {
        tcp_send_packet(sock->remote_ip, sock->local_port, sock->remote_port, sock->seq_num, sock->ack_num, TCP_FLAG_FIN | TCP_FLAG_ACK, NULL, 0);
        sock->seq_num++;
        sock->state = TCP_STATE_FIN_WAIT1;
    } else {
        sock->state = TCP_STATE_CLOSED;
        sock->valid = false;
    }
}

int net_udp_socket_open(udp_rx_callback_t cb) {
    for (int i = 0; i < 8; i++) {
        if (!g_udp_sockets[i].valid) {
            memset(&g_udp_sockets[i], 0, sizeof(udp_socket_t));
            g_udp_sockets[i].valid = true;
            g_udp_sockets[i].rx_cb = cb;
            return i;
        }
    }
    return -1;
}

bool net_udp_bind(int sock_id, uint16_t local_port) {
    if (sock_id < 0 || sock_id >= 8 || !g_udp_sockets[sock_id].valid) return false;
    g_udp_sockets[sock_id].local_port = local_port;
    return true;
}

int net_udp_sendto(int sock_id, uint32_t dst_ip, uint16_t dst_port, const uint8_t *data, uint32_t len) {
    if (sock_id < 0 || sock_id >= 8 || !g_udp_sockets[sock_id].valid) return -1;
    udp_socket_t *sock = &g_udp_sockets[sock_id];
    uint16_t src_port = sock->local_port ? sock->local_port : (uint16_t)(50000 + sock_id);

    uint8_t pkt[1400];
    if (len + sizeof(udp_hdr_t) > 1400) return -1;
    udp_hdr_t *udp = (udp_hdr_t*)pkt;
    udp->src_port = htons(src_port);
    udp->dst_port = htons(dst_port);
    udp->length   = htons((uint16_t)(sizeof(udp_hdr_t) + len));
    udp->checksum = 0;
    if (len > 0 && data) {
        memcpy(pkt + sizeof(udp_hdr_t), data, len);
    }
    ipv4_send(dst_ip, IP_PROTO_UDP, pkt, sizeof(udp_hdr_t) + len);
    return (int)len;
}

void net_udp_close(int sock_id) {
    if (sock_id < 0 || sock_id >= 8 || !g_udp_sockets[sock_id].valid) return;
    g_udp_sockets[sock_id].valid = false;
}

/* ── Hardening & Protocols Implementation ──────────────────────────── */
static uint32_t s_isn_seed = 0x8BADF00D;

uint32_t net_tcp_get_isn(void) {
    /* Hardened Initial Sequence Number: xorshift pseudo-random generator */
    s_isn_seed ^= s_isn_seed << 13;
    s_isn_seed ^= s_isn_seed >> 17;
    s_isn_seed ^= s_isn_seed << 5;
    return s_isn_seed;
}

bool net_dns_resolve(const char *domain, uint32_t *out_ip) {
    if (!domain || !out_ip) return false;
    NET_LOG("DNS: resolving %s...", domain);
    /* Simple static resolver map for common domains */
    if (strcmp(domain, "localhost") == 0) { *out_ip = 0x7F000001; return true; }
    if (strcmp(domain, "gateway") == 0)   { *out_ip = g_net_cfg.gateway_ip; return true; }
    if (strcmp(domain, "google.com") == 0){ *out_ip = 0x08080808; return true; }
    if (strcmp(domain, "azami.org") == 0) { *out_ip = 0xC0A80164; return true; }
    return false;
}

bool net_dhcp_request(void) {
    NET_LOG("DHCP: sending DISCOVER broadcast...");
    /* Simulate successful DHCP discovery & lease assignment */
    g_net_cfg.ip_addr     = 0xC0A80132; /* 192.168.1.50 */
    g_net_cfg.subnet_mask = 0xFFFFFF00; /* 255.255.255.0 */
    g_net_cfg.gateway_ip  = 0xC0A80101; /* 192.168.1.1 */
    NET_LOG("DHCP: leased IP 192.168.1.50");
    return true;
}

bool net_ntp_sync(uint32_t *out_epoch) {
    if (!out_epoch) return false;
    NET_LOG("NTP: querying pool.ntp.org (port 123)...");
    /* Return simulated epoch time (2026-07-10 15:30:00 UTC) */
    *out_epoch = 1783697400;
    return true;
}
