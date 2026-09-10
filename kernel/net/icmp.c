/* ============================================================================
 * AzamiOS — Internet Control Message Protocol (icmp.c)
 * File: kernel/net/icmp.c
 *
 * Implements RFC 792 ICMP Echo Request / Reply (Ping), Destination Unreachable,
 * and Time Exceeded (TTL expired) generation and parsing.
 * ============================================================================ */

#define DEBUG 1
#include "../../include/azami/debug.h"
#include "../../include/azami/defs.h"
#include "../../include/azami/net.h"
#include "../../include/azami/ipv4.h"
#include "../../include/azami/icmp.h"
#include "../../kernel/lib/string.h"

static inline u16 htons(u16 v) { return (u16)((v << 8) | (v >> 8)); }
static inline u16 ntohs(u16 v) { return htons(v); }

void icmp_init(void)
{
    pr_debug("[ICMP] Full ICMP subsystem initialized.\n");
}

void icmp_input(net_buf_t *buf, const ipv4_hdr_t *ip_hdr)
{
    if (!buf || !ip_hdr || buf->len < sizeof(icmp_hdr_t)) {
        if (buf) net_buf_free(buf);
        return;
    }

    const icmp_hdr_t *icmp = (const icmp_hdr_t *)buf->data;

    /* Verify ICMP checksum */
    if (net_checksum(buf->data, buf->len) != 0) {
        pr_debug("[ICMP] Bad ICMP checksum, dropping packet.\n");
        net_buf_free(buf);
        return;
    }

    if (icmp->type == ICMP_TYPE_ECHO_REQUEST) {
        /* Construct and transmit ICMP Echo Reply */
        size_t payload_len = buf->len - sizeof(icmp_hdr_t);
        const u8 *payload = buf->data + sizeof(icmp_hdr_t);

        net_buf_t *rep = net_buf_alloc(NET_BUF_HEADROOM + sizeof(icmp_hdr_t) + payload_len);
        if (!rep) {
            net_buf_free(buf);
            return;
        }

        net_buf_reserve(rep, NET_BUF_HEADROOM);
        icmp_hdr_t *rep_icmp = (icmp_hdr_t *)net_buf_put(rep, sizeof(icmp_hdr_t));
        rep_icmp->type = ICMP_TYPE_ECHO_REPLY;
        rep_icmp->code = 0;
        rep_icmp->checksum = 0;
        rep_icmp->id = icmp->id;
        rep_icmp->seq = icmp->seq;

        if (payload_len > 0) {
            void *rep_payload = net_buf_put(rep, payload_len);
            memcpy(rep_payload, payload, payload_len);
        }

        rep_icmp->checksum = net_checksum(rep->data, rep->len);

        /* Send reply back to original sender IP */
        ipv4_send(rep, ip_hdr->src_ip, IP_PROTO_ICMP);
    } else if (icmp->type == ICMP_TYPE_ECHO_REPLY) {
        pr_debug("[ICMP] Received Echo Reply from %u.%u.%u.%u (id=0x%x, seq=%u, len=%zu)\n",
                 ip_hdr->src_ip[0], ip_hdr->src_ip[1], ip_hdr->src_ip[2], ip_hdr->src_ip[3],
                 ntohs(icmp->id), ntohs(icmp->seq), buf->len);
    }

    net_buf_free(buf);
}

s64 icmp_send_echo(const u8 target_ip[4], u16 id, u16 seq, const void *payload, size_t payload_len)
{
    if (!target_ip) return -1;
    if (payload_len == 0) payload_len = 32;

    net_buf_t *buf = net_buf_alloc(NET_BUF_HEADROOM + sizeof(icmp_hdr_t) + payload_len);
    if (!buf) return -1;

    net_buf_reserve(buf, NET_BUF_HEADROOM);
    icmp_hdr_t *icmp = (icmp_hdr_t *)net_buf_put(buf, sizeof(icmp_hdr_t));
    icmp->type = ICMP_TYPE_ECHO_REQUEST;
    icmp->code = 0;
    icmp->checksum = 0;
    icmp->id = htons(id);
    icmp->seq = htons(seq);

    u8 *pdata = (u8 *)net_buf_put(buf, payload_len);
    if (payload) {
        memcpy(pdata, payload, payload_len);
    } else {
        for (size_t i = 0; i < payload_len; i++) pdata[i] = (u8)('a' + (i % 26));
    }

    icmp->checksum = net_checksum(buf->data, buf->len);

    return ipv4_send(buf, target_ip, IP_PROTO_ICMP);
}

/*
 * RFC 792 asks for the offending IP header plus the first 8 bytes of its
 * payload.  `orig_data_len` says how many of those 8 bytes actually exist:
 * the quote used to be an unconditional 8-byte copy, so a 20-byte datagram
 * carrying an unrecognised protocol number — which leaves no payload at all
 * once the header is pulled — read 8 bytes past the end of the packet buffer.
 * Any remote host could trigger that with a single packet.
 */
static size_t icmp_quote(net_buf_t *buf, const ipv4_hdr_t *orig_ip,
                         const void *orig_data, size_t orig_data_len)
{
    void *dst_data = net_buf_put(buf, sizeof(ipv4_hdr_t));
    if (!dst_data) return 0;
    memcpy(dst_data, orig_ip, sizeof(ipv4_hdr_t));

    size_t quote = (orig_data_len < 8) ? orig_data_len : 8;
    if (orig_data && quote > 0) {
        void *dst_extra = net_buf_put(buf, quote);
        if (dst_extra) memcpy(dst_extra, orig_data, quote);
    }
    return quote;
}

void icmp_send_dest_unreach(const ipv4_hdr_t *orig_ip, const void *orig_data,
                            size_t orig_data_len, u8 code)
{
    if (!orig_ip) return;

    size_t copy_len = sizeof(ipv4_hdr_t) + 8;

    net_buf_t *buf = net_buf_alloc(NET_BUF_HEADROOM + sizeof(icmp_hdr_t) + copy_len);
    if (!buf) return;

    net_buf_reserve(buf, NET_BUF_HEADROOM);
    icmp_hdr_t *icmp = (icmp_hdr_t *)net_buf_put(buf, sizeof(icmp_hdr_t));
    icmp->type = ICMP_TYPE_DEST_UNREACH;
    icmp->code = code;
    icmp->checksum = 0;
    icmp->id = 0;
    icmp->seq = 0;

    icmp_quote(buf, orig_ip, orig_data, orig_data_len);

    icmp->checksum = net_checksum(buf->data, buf->len);

    ipv4_send(buf, orig_ip->src_ip, IP_PROTO_ICMP);
}

void icmp_send_time_exceeded(const ipv4_hdr_t *orig_ip, const void *orig_data,
                             size_t orig_data_len)
{
    if (!orig_ip) return;

    size_t copy_len = sizeof(ipv4_hdr_t) + 8;

    net_buf_t *buf = net_buf_alloc(NET_BUF_HEADROOM + sizeof(icmp_hdr_t) + copy_len);
    if (!buf) return;

    net_buf_reserve(buf, NET_BUF_HEADROOM);
    icmp_hdr_t *icmp = (icmp_hdr_t *)net_buf_put(buf, sizeof(icmp_hdr_t));
    icmp->type = ICMP_TYPE_TIME_EXCEEDED;
    icmp->code = ICMP_CODE_TTL_EXCEEDED;
    icmp->checksum = 0;
    icmp->id = 0;
    icmp->seq = 0;

    icmp_quote(buf, orig_ip, orig_data, orig_data_len);

    icmp->checksum = net_checksum(buf->data, buf->len);

    ipv4_send(buf, orig_ip->src_ip, IP_PROTO_ICMP);
}
