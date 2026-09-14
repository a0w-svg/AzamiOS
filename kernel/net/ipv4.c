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
#include "../../include/azami/igmp.h"
#include "../../include/azami/arp.h"
#include "../../include/azami/udp.h"
#include "../../include/azami/tcp.h"
#include "../../kernel/mm/kmalloc.h"
#include "../../arch/x86_64/cpu/spinlock.h"
#include "../../kernel/lib/string.h"

static route_entry_t g_routes[MAX_ROUTES];
static spinlock_t    g_route_lock = SPINLOCK_INIT;
static u16        g_ip_id_counter = 1;

/* ── Fragment Reassembly (RFC 791 §3.2) ───────────────────────────────────
 *
 * ipv4_input() used to drop every fragmented datagram outright rather than
 * pass a half-parsed one up to a transport handler that expects a full
 * segment. That is still true for any individual fragment on its own; the
 * difference is that fragments are now collected here until either the set
 * is complete (the reassembled payload goes up exactly like an
 * unfragmented one) or IP_REASSEMBLY_TIMEOUT passes without the missing
 * piece turning up (the whole set is discarded).
 *
 * Each context tracks which byte ranges have arrived as a small unsorted
 * list rather than a bitmap — IP_MAX_FRAGMENTS is generous for anything
 * this stack is likely to originate or receive, and completion is only
 * ever checked (an O(n log n) sort-and-walk) once the terminal fragment
 * (MF=0) has told us the final length, not on every arrival. */
#define IP_REASSEMBLY_MAX     4
#define IP_REASSEMBLY_TIMEOUT 30      /* seconds without a new fragment before giving up */
#define IP_MAX_DGRAM_LEN      65535
#define IP_MAX_FRAGMENTS      64

typedef struct { u16 offset; u16 len; } ip_frag_range_t;

typedef struct {
    bool            in_use;
    u8              src_ip[4];
    u8              dst_ip[4];
    u16             id;
    u8              protocol;
    u16             total_len;   /* 0 until the final (MF=0) fragment has been seen */
    u8             *data;        /* IP_MAX_DGRAM_LEN bytes, allocated per active context */
    ip_frag_range_t ranges[IP_MAX_FRAGMENTS];
    int             nranges;
    u32             timestamp;
} ip_reasm_ctx_t;

static ip_reasm_ctx_t g_reasm[IP_REASSEMBLY_MAX];
static spinlock_t     g_reasm_lock = SPINLOCK_INIT;
static u32            g_ipv4_ticks = 0;

/* g_route_lock uses spinlock_lock_irqsave()/_irqrestore() throughout this
 * section, for the same reason as tcp.c's/udp.c's/dhcp.c's identical note:
 * route_add() is called directly from dhcp_input(), which — like the rest
 * of the input path — runs from a timer interrupt on any CPU, while
 * route_lookup() and route_add()/route_del() are also reached in ordinary
 * process context (ipv4_send() from a socket syscall; the SIOCSIFADDR/
 * SIOCSIFGW ioctls via net_set_ip()/net_set_gateway()). A plain
 * spinlock_lock() left interrupts enabled across those process-context
 * critical sections, so the same timer interrupt could land on the CPU
 * already holding this lock and spin forever inside dhcp_input()'s own
 * acquire. See kernel/net/tcp.c for the full writeup. */

static inline u16 htons(u16 v) { return (u16)((v << 8) | (v >> 8)); }
static inline u16 ntohs(u16 v) { return htons(v); }

/* ── Routing Table Subsystem ──────────────────────────────────────────────── */

void route_init(void)
{
    irqflags_t flags = spinlock_lock_irqsave(&g_route_lock);
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

    spinlock_unlock_irqrestore(&g_route_lock, flags);
    pr_debug("[IPv4] Routing table initialized with local loopback.\n");
}

int route_add(const u8 dst[4], const u8 mask[4], const u8 gw[4], struct net_device *dev, u32 flags_in, u32 metric)
{
    if (!dst || !mask) return -1;
    irqflags_t flags = spinlock_lock_irqsave(&g_route_lock);

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
        spinlock_unlock_irqrestore(&g_route_lock, flags);
        return -1; /* Table full */
    }

    memcpy(g_routes[slot].dst, dst, 4);
    memcpy(g_routes[slot].mask, mask, 4);
    if (gw) memcpy(g_routes[slot].gateway, gw, 4);
    else memset(g_routes[slot].gateway, 0, 4);
    g_routes[slot].dev = dev ? dev : net_get_default_device();
    g_routes[slot].flags = flags_in | RT_FLAG_UP;
    g_routes[slot].metric = metric;

    spinlock_unlock_irqrestore(&g_route_lock, flags);
    return 0;
}

int route_del(const u8 dst[4], const u8 mask[4])
{
    if (!dst || !mask) return -1;
    irqflags_t flags = spinlock_lock_irqsave(&g_route_lock);

    for (int i = 0; i < MAX_ROUTES; i++) {
        if (g_routes[i].flags & RT_FLAG_UP) {
            if (memcmp(g_routes[i].dst, dst, 4) == 0 &&
                memcmp(g_routes[i].mask, mask, 4) == 0) {
                g_routes[i].flags = 0;
                spinlock_unlock_irqrestore(&g_route_lock, flags);
                return 0;
            }
        }
    }

    spinlock_unlock_irqrestore(&g_route_lock, flags);
    return -1;
}

int route_lookup(const u8 dst_ip[4], u8 next_hop_out[4], struct net_device **dev_out)
{
    if (!dst_ip || !next_hop_out || !dev_out) return -1;

    irqflags_t flags = spinlock_lock_irqsave(&g_route_lock);

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
        spinlock_unlock_irqrestore(&g_route_lock, flags);
        return -1; /* No route to host */
    }

    route_entry_t *r = &g_routes[best_match];
    if (r->flags & RT_FLAG_GATEWAY) {
        memcpy(next_hop_out, r->gateway, 4);
    } else {
        memcpy(next_hop_out, dst_ip, 4); /* Direct delivery on local segment */
    }

    *dev_out = r->dev ? r->dev : net_get_default_device();
    spinlock_unlock_irqrestore(&g_route_lock, flags);

    return 0;
}

void route_print_table(void)
{
    irqflags_t flags = spinlock_lock_irqsave(&g_route_lock);
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
    spinlock_unlock_irqrestore(&g_route_lock, flags);
}

/* Same address encoding as tcp_format_proc_net()'s comment (kernel/net/
 * tcp.c) — the raw in_addr bytes read as a little-endian u32. */
static u32 route_proc_net_field(const u8 ip[4])
{
    return (u32)ip[0] | ((u32)ip[1] << 8) | ((u32)ip[2] << 16) | ((u32)ip[3] << 24);
}

size_t route_format_proc_net(char *buf, size_t max)
{
    size_t off = (size_t)scnprintf(buf, max,
        "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT\n");

    irqflags_t flags = spinlock_lock_irqsave(&g_route_lock);
    for (int i = 0; i < MAX_ROUTES; i++) {
        if (!(g_routes[i].flags & RT_FLAG_UP)) continue;
        route_entry_t *r = &g_routes[i];
        u16 rflags = 0x0001; /* RTF_UP */
        if (r->flags & RT_FLAG_GATEWAY) rflags |= 0x0002; /* RTF_GATEWAY */
        off += (size_t)scnprintf(buf + off, max > off ? max - off : 0,
            "%s\t%08X\t%08X\t%04X\t0\t0\t%u\t%08X\t0\t0\t0\n",
            r->dev ? r->dev->name : "net0",
            route_proc_net_field(r->dst), route_proc_net_field(r->gateway),
            rflags, r->metric, route_proc_net_field(r->mask));
    }
    spinlock_unlock_irqrestore(&g_route_lock, flags);
    return off;
}

/* ── Fragment Reassembly Helpers ──────────────────────────────────────────── */

static void reasm_ctx_release(ip_reasm_ctx_t *ctx)
{
    if (ctx->data) { kfree(ctx->data); ctx->data = NULL; }
    ctx->in_use = false;
    ctx->nranges = 0;
    ctx->total_len = 0;
}

/* Selection-sorts ranges by offset (n <= IP_MAX_FRAGMENTS, run only once
 * total_len is known) then walks them checking for gaps. A duplicate or
 * overlapping fragment is harmless here — it just narrows or repeats a span
 * `covered` already reaches. */
static bool reasm_ctx_complete(ip_reasm_ctx_t *ctx)
{
    if (ctx->total_len == 0) return false;

    for (int i = 0; i < ctx->nranges; i++) {
        int min_i = i;
        for (int j = i + 1; j < ctx->nranges; j++) {
            if (ctx->ranges[j].offset < ctx->ranges[min_i].offset) min_i = j;
        }
        if (min_i != i) {
            ip_frag_range_t t = ctx->ranges[i];
            ctx->ranges[i] = ctx->ranges[min_i];
            ctx->ranges[min_i] = t;
        }
    }

    u32 covered = 0;
    for (int i = 0; i < ctx->nranges; i++) {
        if (ctx->ranges[i].offset > covered) return false; /* gap before this fragment */
        u32 end = (u32)ctx->ranges[i].offset + ctx->ranges[i].len;
        if (end > covered) covered = end;
    }
    return covered >= ctx->total_len;
}

/*
 * Folds one fragment into its datagram's reassembly context, keyed by
 * (src, dst, id, protocol) per RFC 791. `buf` holds just this fragment's
 * payload (the caller has already pulled the IP header) and is always
 * consumed — freed here whether or not the set completes. Returns a new
 * net_buf_t holding the full reassembled payload once the last gap closes,
 * NULL otherwise (either still incomplete, or dropped as malformed).
 */
static net_buf_t *ip_reassembly_input(net_buf_t *buf, const ipv4_hdr_t *ip, u16 frag_field)
{
    u32 offset = (u32)(frag_field & IP_FRAG_OFF_MASK) * 8;
    bool more = (frag_field & IP_FLAG_MF) != 0;
    size_t flen = buf->len;

    if (offset + flen > IP_MAX_DGRAM_LEN) {
        pr_debug("[IPv4] Fragment offset overflow (off=%u len=%zu), dropping.\n", offset, flen);
        net_buf_free(buf);
        return NULL;
    }

    irqflags_t irqf = spinlock_lock_irqsave(&g_reasm_lock);

    ip_reasm_ctx_t *ctx = NULL;
    ip_reasm_ctx_t *free_slot = NULL;
    ip_reasm_ctx_t *oldest = NULL;
    for (int i = 0; i < IP_REASSEMBLY_MAX; i++) {
        ip_reasm_ctx_t *c = &g_reasm[i];
        if (c->in_use) {
            if (c->id == ntohs(ip->id) && c->protocol == ip->protocol &&
                memcmp(c->src_ip, ip->src_ip, 4) == 0 &&
                memcmp(c->dst_ip, ip->dst_ip, 4) == 0) {
                ctx = c;
                break;
            }
            if (!oldest || c->timestamp < oldest->timestamp) oldest = c;
        } else if (!free_slot) {
            free_slot = c;
        }
    }

    bool is_new = false;
    if (!ctx) {
        /* No in-flight reassembly matches: use a free slot, or — table full
         * — evict whichever context has gone longest without a fragment. */
        ctx = free_slot ? free_slot : oldest;
        is_new = true;
    }

    if (is_new) {
        if (ctx->in_use) reasm_ctx_release(ctx);
        ctx->data = (u8 *)kmalloc(IP_MAX_DGRAM_LEN);
        if (!ctx->data) {
            spinlock_unlock_irqrestore(&g_reasm_lock, irqf);
            net_buf_free(buf);
            return NULL;
        }
        ctx->in_use = true;
        memcpy(ctx->src_ip, ip->src_ip, 4);
        memcpy(ctx->dst_ip, ip->dst_ip, 4);
        ctx->id = ntohs(ip->id);
        ctx->protocol = ip->protocol;
        ctx->nranges = 0;
        ctx->total_len = 0;
    }

    memcpy(ctx->data + offset, buf->data, flen);
    if (ctx->nranges < IP_MAX_FRAGMENTS) {
        ctx->ranges[ctx->nranges].offset = (u16)offset;
        ctx->ranges[ctx->nranges].len = (u16)flen;
        ctx->nranges++;
    } /* else: pathologically fragmented datagram — the bytes are still
       * written above, but this fragment's range record is dropped, which
       * can only make reasm_ctx_complete() stricter, never wrongly "done". */
    if (!more) ctx->total_len = (u16)(offset + flen);
    ctx->timestamp = g_ipv4_ticks;

    net_buf_free(buf); /* copied into ctx->data; the per-fragment buffer is done */

    net_buf_t *result = NULL;
    if (reasm_ctx_complete(ctx)) {
        result = net_buf_alloc(NET_BUF_HEADROOM + ctx->total_len);
        if (result) {
            net_buf_reserve(result, NET_BUF_HEADROOM);
            void *p = net_buf_put(result, ctx->total_len);
            memcpy(p, ctx->data, ctx->total_len);
        }
        reasm_ctx_release(ctx);
    }

    spinlock_unlock_irqrestore(&g_reasm_lock, irqf);
    return result;
}

void ipv4_timer_tick(void)
{
    irqflags_t irqf = spinlock_lock_irqsave(&g_reasm_lock);
    g_ipv4_ticks++;
    for (int i = 0; i < IP_REASSEMBLY_MAX; i++) {
        if (g_reasm[i].in_use && (g_ipv4_ticks - g_reasm[i].timestamp) > IP_REASSEMBLY_TIMEOUT) {
            reasm_ctx_release(&g_reasm[i]);
        }
    }
    spinlock_unlock_irqrestore(&g_reasm_lock, irqf);
}

/* ── IPv4 Transmission & Ingestion ────────────────────────────────────────── */

void ipv4_init(void)
{
    route_init();
    icmp_init();
    igmp_init();
    pr_debug("[IPv4] Layer 3 IPv4 engine initialized.\n");
}

static bool ipv4_is_broadcast(const u8 dst_ip[4])
{
    return dst_ip[0] == 255 && dst_ip[1] == 255 && dst_ip[2] == 255 && dst_ip[3] == 255;
}

static bool ipv4_is_multicast(const u8 dst_ip[4])
{
    return (dst_ip[0] & 0xF0) == 0xE0; /* 224.0.0.0/4 */
}

/*
 * Takes a fully-formed IP datagram (header already at buf->data, checksummed,
 * no Ethernet header yet) and gets it onto the wire: local loopback bypass,
 * broadcast/multicast direct-MAC framing, or unicast ARP resolution — the
 * tail half ipv4_send() always did for its one packet, pulled out so
 * ipv4_fragment_and_send() can run it once per fragment and
 * ipv4_send_prebuilt() can run it for an IP_HDRINCL raw send without
 * duplicating the ARP/Ethernet dispatch.
 */
static int ipv4_deliver(net_buf_t *buf, const u8 dst_ip[4], bool is_loopback)
{
    if (is_loopback) {
        net_loopback_input(buf);
        return 0;
    }

    bool is_bcast = ipv4_is_broadcast(dst_ip);
    bool is_mcast = !is_bcast && ipv4_is_multicast(dst_ip);

    u8 next_hop[4];
    struct net_device *dev = NULL;

    if (is_bcast || is_mcast) {
        /* Neither needs a routed next-hop: broadcast and multicast frames
         * always go out the local link directly. */
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

    eth_hdr_t *eth = (eth_hdr_t *)net_buf_push(buf, sizeof(eth_hdr_t));
    if (!eth) {
        net_buf_free(buf);
        return -1;
    }

    memcpy(eth->src, dev->mac, 6);
    eth->ethertype = htons(ETH_P_IP);

    if (is_bcast) {
        memset(eth->dst, 0xFF, 6);
        dev->send(buf->data, buf->len);
        net_buf_free(buf);
        return 0;
    }

    if (is_mcast) {
        /* RFC 1112 §6.4: the low-order 23 bits of the group address map
         * directly onto an 01:00:5e:xx:xx:xx Ethernet address — no ARP;
         * every host that has joined the group is simply listening for it. */
        eth->dst[0] = 0x01; eth->dst[1] = 0x00; eth->dst[2] = 0x5e;
        eth->dst[3] = dst_ip[1] & 0x7F;
        eth->dst[4] = dst_ip[2];
        eth->dst[5] = dst_ip[3];
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

/*
 * Splits an over-MTU IP datagram (header already built by ipv4_send(), with
 * its L4 checksum — if any — already computed over the *whole* unfragmented
 * payload, exactly as RFC 791 requires) into MTU-sized fragments and hands
 * each to ipv4_deliver() as its own packet. `buf` is always consumed.
 */
static int ipv4_fragment_and_send(net_buf_t *buf, const u8 dst_ip[4], u32 mtu)
{
    ipv4_hdr_t hdr_template;
    memcpy(&hdr_template, buf->data, sizeof(ipv4_hdr_t));
    size_t payload_len = buf->len - sizeof(ipv4_hdr_t);

    if (ntohs(hdr_template.frag_offset) & IP_FLAG_DF) {
        icmp_send_dest_unreach(&hdr_template, buf->data + sizeof(ipv4_hdr_t),
                               payload_len > 8 ? 8 : payload_len, ICMP_CODE_FRAG_NEEDED);
        net_buf_free(buf);
        return -1;
    }

    size_t max_payload = ((mtu - sizeof(ipv4_hdr_t)) / 8) * 8;
    if (max_payload == 0) {
        pr_debug("[IPv4] MTU %u too small to fragment into, dropping.\n", mtu);
        net_buf_free(buf);
        return -1;
    }

    /* Copy the payload out before freeing buf: each fragment is built fresh,
     * with its own headroom for the Ethernet header ipv4_deliver() pushes,
     * not a slice of the original buffer. */
    u8 *payload = (u8 *)kmalloc(payload_len);
    if (!payload) {
        net_buf_free(buf);
        return -1;
    }
    memcpy(payload, buf->data + sizeof(ipv4_hdr_t), payload_len);
    net_buf_free(buf);

    int rc = 0;
    size_t offset = 0;
    while (offset < payload_len) {
        size_t chunk = payload_len - offset;
        bool more = false;
        if (chunk > max_payload) { chunk = max_payload; more = true; }
        else if (offset + chunk < payload_len) more = true;

        net_buf_t *frag = net_buf_alloc(NET_BUF_HEADROOM + sizeof(ipv4_hdr_t) + chunk);
        if (!frag) { rc = -1; break; }
        net_buf_reserve(frag, NET_BUF_HEADROOM);

        ipv4_hdr_t *fh = (ipv4_hdr_t *)net_buf_put(frag, sizeof(ipv4_hdr_t));
        *fh = hdr_template;
        fh->total_len = htons((u16)(sizeof(ipv4_hdr_t) + chunk));
        fh->frag_offset = htons((u16)((offset / 8) | (more ? IP_FLAG_MF : 0)));
        fh->checksum = 0;

        void *fp = net_buf_put(frag, chunk);
        memcpy(fp, payload + offset, chunk);
        fh->checksum = net_checksum(fh, sizeof(ipv4_hdr_t));

        if (ipv4_deliver(frag, dst_ip, false) < 0) rc = -1;

        offset += chunk;
    }

    kfree(payload);
    return rc;
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
        return ipv4_deliver(buf, dst_ip, true);
    }

    /*
     * 3. Fragment if this datagram won't fit the outgoing device's MTU.
     * Finding out which device that is means the same routing decision
     * ipv4_deliver() (or each of ipv4_fragment_and_send()'s per-fragment
     * calls into it) makes on its own — one more small routing-table scan,
     * not a real cost, and it keeps the ARP/Ethernet dispatch logic in the
     * one place rather than duplicated here.
     */
    net_device_t *mtu_dev = NULL;
    if (ipv4_is_broadcast(dst_ip) || ipv4_is_multicast(dst_ip)) {
        mtu_dev = net_get_default_device();
    } else {
        u8 unused_hop[4];
        route_lookup(dst_ip, unused_hop, &mtu_dev);
    }
    u32 mtu = (mtu_dev && mtu_dev->mtu >= 68) ? mtu_dev->mtu : 1500;

    if (buf->len <= mtu) {
        return ipv4_deliver(buf, dst_ip, false);
    }
    return ipv4_fragment_and_send(buf, dst_ip, mtu);
}

int ipv4_send_prebuilt(net_buf_t *buf)
{
    if (!buf || buf->len < sizeof(ipv4_hdr_t)) {
        if (buf) net_buf_free(buf);
        return -1;
    }

    ipv4_hdr_t *ip = (ipv4_hdr_t *)buf->data;
    if ((ip->ihl_version >> 4) != 4) {
        net_buf_free(buf);
        return -1;
    }
    size_t ihl_bytes = (size_t)(ip->ihl_version & 0x0F) * 4;
    if (ihl_bytes < sizeof(ipv4_hdr_t) || buf->len < ihl_bytes) {
        net_buf_free(buf);
        return -1;
    }

    /* IP_HDRINCL trusts the caller for source address, TTL, flags and
     * everything else, but a checksum left at zero (the common case — most
     * callers don't bother) still needs to be real before this goes out. */
    if (ip->checksum == 0) {
        ip->checksum = net_checksum(ip, ihl_bytes);
    }

    u8 dst_ip[4];
    memcpy(dst_ip, ip->dst_ip, 4);
    bool is_loopback = (dst_ip[0] == 127);

    /* Not fragmented: a caller building its own IP header is assumed to
     * also own keeping it under the path MTU (see ipv4_send_prebuilt()'s
     * comment in ipv4.h). */
    return ipv4_deliver(buf, dst_ip, is_loopback);
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

    u16 frag = ntohs(ip->frag_offset);

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

    /* 224.0.0.1 (All-Hosts) is implicit membership per RFC 1112 §7 — every
     * host answers it without an explicit join; anything else in
     * 224.0.0.0/4 is accepted only once ip_multicast_join() (IP_ADD_
     * MEMBERSHIP) has actually asked for that group. */
    bool is_mcast_for_us = ipv4_is_multicast(ip->dst_ip) &&
        ((ip->dst_ip[0] == 224 && ip->dst_ip[1] == 0 && ip->dst_ip[2] == 0 && ip->dst_ip[3] == 1) ||
         ip_multicast_is_member(ip->dst_ip));

    if (!is_unconfigured && !is_loop && !is_bcast && !is_for_us && !is_mcast_for_us) {
        net_buf_free(buf);
        return;
    }

    /* Strip IPv4 header from buffer payload */
    ipv4_hdr_t ip_copy;
    memcpy(&ip_copy, ip, sizeof(ipv4_hdr_t));
    net_buf_pull(buf, ihl_bytes);

    /*
     * Fragments are folded into their datagram's reassembly context instead
     * of being dropped outright — see ip_reassembly_input(). Every fragment
     * but the last returns NULL (nothing to dispatch yet, and this buf has
     * already been consumed); once the set is complete, `buf` is replaced
     * with the full reassembled payload and processing falls through to the
     * same raw/protocol dispatch an unfragmented datagram gets.
     */
    if ((frag & IP_FLAG_MF) || (frag & IP_FRAG_OFF_MASK)) {
        net_buf_t *reassembled = ip_reassembly_input(buf, &ip_copy, frag);
        if (!reassembled) return;
        buf = reassembled;
    }

    /* Dispatch copy to RAW sockets */
    raw_input(buf, &ip_copy);

    /* Dispatch to Layer 4 transport protocol handlers */
    if (ip_copy.protocol == IP_PROTO_ICMP) {
        icmp_input(buf, &ip_copy);
    } else if (ip_copy.protocol == IP_PROTO_IGMP) {
        igmp_input(buf, &ip_copy);
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
