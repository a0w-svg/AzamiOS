/* ============================================================================
 * AzamiOS — AF_NETLINK / NETLINK_ROUTE Engine (netlink.c)
 * File: kernel/net/netlink.c
 *
 * A read-only rtnetlink responder: RTM_GETLINK/GETADDR/GETROUTE dump
 * requests get a real answer built from live net_device_t/route_entry_t
 * state, in Linux's wire format, so an unmodified netlink-based tool can
 * query this kernel the way it would query a real one. Anything that would
 * *change* link/address/route state (RTM_NEWADDR, RTM_NEWROUTE, ...) is
 * answered with NLMSG_ERROR/-EOPNOTSUPP rather than silently doing nothing —
 * see netlink_socket_send()'s default case.
 * ============================================================================ */

#define DEBUG 1
#include "../../include/azami/debug.h"
#include "../../include/azami/defs.h"
#include "../../include/azami/net.h"
#include "../../include/azami/ipv4.h"
#include "../../include/azami/netlink.h"
#include "../../include/azami/socket.h"
#include "../../kernel/mm/kmalloc.h"
#include "../../kernel/sched/sched.h"
#include "../../kernel/lib/string.h"

static netlink_sock_t *g_nl_sockets = NULL;
static spinlock_t      g_nl_lock = SPINLOCK_INIT;

/* Generous enough for a handful of interfaces/addresses/routes with room to
 * spare — this kernel has one physical interface plus loopback, and
 * MAX_ROUTES tops out at 32 (include/azami/ipv4.h). */
#define NL_RESP_BUF_SIZE 8192

netlink_sock_t *netlink_socket_create(void)
{
    netlink_sock_t *nl = (netlink_sock_t *)kzalloc(sizeof(netlink_sock_t));
    if (!nl) return NULL;
    net_buf_queue_init(&nl->rx_queue);
    spinlock_init(&nl->lock);
    nl->wait_thread = NULL;

    irqflags_t flags = spinlock_lock_irqsave(&g_nl_lock);
    nl->next = g_nl_sockets;
    g_nl_sockets = nl;
    spinlock_unlock_irqrestore(&g_nl_lock, flags);
    return nl;
}

void netlink_socket_close(netlink_sock_t *nl)
{
    if (!nl) return;

    irqflags_t list_flags = spinlock_lock_irqsave(&g_nl_lock);
    netlink_sock_t **curr = &g_nl_sockets;
    while (*curr) {
        if (*curr == nl) {
            *curr = nl->next;
            break;
        }
        curr = &(*curr)->next;
    }
    spinlock_unlock_irqrestore(&g_nl_lock, list_flags);

    irqflags_t sock_flags = spinlock_lock_irqsave(&nl->lock);
    net_buf_queue_purge(&nl->rx_queue);
    if (nl->wait_thread) {
        sched_unblock(nl->wait_thread);
        nl->wait_thread = NULL;
    }
    spinlock_unlock_irqrestore(&nl->lock, sock_flags);
    kfree(nl);
}

/* ── Message-building helpers ─────────────────────────────────────────────
 *
 * Every fixed struct here (nlmsghdr_t, ifinfomsg_t, ifaddrmsg_t, rtmsg_t) is
 * already a multiple of 4 bytes, and RTA_ALIGN() pads every attribute the
 * same way, so `off` stays 4-byte aligned throughout without this needing
 * to track alignment explicitly — NLMSG_ALIGN() would be a no-op here, not
 * a fix for anything.
 */

static void nl_put_attr(u8 *base, size_t *off, u16 rta_type, const void *data, size_t data_len)
{
    rtattr_t rta;
    rta.rta_len = (u16)(sizeof(rtattr_t) + data_len);
    rta.rta_type = rta_type;
    memcpy(base + *off, &rta, sizeof(rta));
    if (data && data_len) memcpy(base + *off + sizeof(rta), data, data_len);
    *off += RTA_ALIGN(rta.rta_len);
}

static void nl_begin_msg(u8 *base, size_t *off, u16 type, u16 nlflags, u32 seq, u32 pid, size_t *hdr_off)
{
    nlmsghdr_t nh;
    memset(&nh, 0, sizeof(nh));
    nh.nlmsg_type = type;
    nh.nlmsg_flags = nlflags;
    nh.nlmsg_seq = seq;
    nh.nlmsg_pid = pid;
    *hdr_off = *off;
    memcpy(base + *off, &nh, sizeof(nh));
    *off += sizeof(nh);
}

static void nl_end_msg(u8 *base, size_t hdr_off, size_t *off)
{
    nlmsghdr_t *nh = (nlmsghdr_t *)(base + hdr_off);
    nh->nlmsg_len = (u32)(*off - hdr_off);
}

static int mask_to_prefix(const u8 mask[4])
{
    int bits = 0;
    for (int i = 0; i < 4; i++) {
        u8 m = mask[i];
        while (m) { bits += (m & 1); m >>= 1; }
    }
    return bits;
}

static size_t nl_build_getlink(u8 *buf, size_t buf_size, u32 seq, u32 pid)
{
    size_t off = 0;
    int count = net_get_device_count();

    for (int i = 0; i < count; i++) {
        net_device_t *dev = net_get_device_by_index(i);
        if (!dev || off + 256 > buf_size) continue;

        size_t hdr_off;
        nl_begin_msg(buf, &off, RTM_NEWLINK, NLM_F_MULTI, seq, pid, &hdr_off);

        ifinfomsg_t ifi;
        memset(&ifi, 0, sizeof(ifi));
        ifi.ifi_family = AF_UNSPEC;
        ifi.ifi_type = 1; /* ARPHRD_ETHER — close enough for lo too here */
        ifi.ifi_index = i;
        ifi.ifi_flags = dev->flags;
        ifi.ifi_change = 0xFFFFFFFF;
        memcpy(buf + off, &ifi, sizeof(ifi));
        off += sizeof(ifi);

        nl_put_attr(buf, &off, IFLA_IFNAME, dev->name, strlen(dev->name) + 1);
        nl_put_attr(buf, &off, IFLA_ADDRESS, dev->mac, 6);
        u32 mtu = dev->mtu;
        nl_put_attr(buf, &off, IFLA_MTU, &mtu, sizeof(mtu));

        nl_end_msg(buf, hdr_off, &off);
    }

    size_t hdr_off;
    nl_begin_msg(buf, &off, NLMSG_DONE, NLM_F_MULTI, seq, pid, &hdr_off);
    nl_end_msg(buf, hdr_off, &off);
    return off;
}

static size_t nl_build_getaddr(u8 *buf, size_t buf_size, u32 seq, u32 pid)
{
    size_t off = 0;
    int count = net_get_device_count();

    for (int i = 0; i < count; i++) {
        net_device_t *dev = net_get_device_by_index(i);
        if (!dev) continue;

        /* dev->ip/netmask/broadcast are the driver's registration-time
         * values and are never updated after DHCP configures an address —
         * net_set_ip()/net_set_netmask() (net.c) only ever touch the global
         * g_host_ip/g_host_netmask/etc SIOCGIFADDR already reads from. lo0
         * is the one device whose struct fields *are* the real, permanent
         * answer (net_init() sets them once and nothing ever changes them),
         * so it's the only one read directly here. */
        u8 ip[4], mask[4], brd[4];
        if (dev->flags & IFF_LOOPBACK) {
            memcpy(ip, dev->ip, 4);
            memcpy(mask, dev->netmask, 4);
            memcpy(brd, dev->broadcast, 4);
        } else {
            net_get_ip(ip);
            net_get_netmask(mask);
            for (int k = 0; k < 4; k++) brd[k] = (u8)(ip[k] | (u8)~mask[k]);
        }

        bool has_ip = (ip[0] || ip[1] || ip[2] || ip[3]);
        if (!has_ip || off + 256 > buf_size) continue;

        size_t hdr_off;
        nl_begin_msg(buf, &off, RTM_NEWADDR, NLM_F_MULTI, seq, pid, &hdr_off);

        ifaddrmsg_t ifa;
        memset(&ifa, 0, sizeof(ifa));
        ifa.ifa_family = AF_INET;
        ifa.ifa_prefixlen = (u8)mask_to_prefix(mask);
        ifa.ifa_scope = (dev->flags & IFF_LOOPBACK) ? RT_SCOPE_HOST : RT_SCOPE_UNIVERSE;
        ifa.ifa_index = i;
        memcpy(buf + off, &ifa, sizeof(ifa));
        off += sizeof(ifa);

        nl_put_attr(buf, &off, IFA_ADDRESS, ip, 4);
        nl_put_attr(buf, &off, IFA_LOCAL, ip, 4);
        nl_put_attr(buf, &off, IFA_BROADCAST, brd, 4);
        nl_put_attr(buf, &off, IFA_LABEL, dev->name, strlen(dev->name) + 1);

        nl_end_msg(buf, hdr_off, &off);
    }

    size_t hdr_off;
    nl_begin_msg(buf, &off, NLMSG_DONE, NLM_F_MULTI, seq, pid, &hdr_off);
    nl_end_msg(buf, hdr_off, &off);
    return off;
}

static size_t nl_build_getroute(u8 *buf, size_t buf_size, u32 seq, u32 pid)
{
    size_t off = 0;
    route_entry_t routes[32]; /* MAX_ROUTES, kept local so this file needn't include the count constant */
    int n = route_get_all(routes, 32);

    for (int i = 0; i < n; i++) {
        if (off + 256 > buf_size) break;
        route_entry_t *r = &routes[i];

        size_t hdr_off;
        nl_begin_msg(buf, &off, RTM_NEWROUTE, NLM_F_MULTI, seq, pid, &hdr_off);

        rtmsg_t rtm;
        memset(&rtm, 0, sizeof(rtm));
        rtm.rtm_family = AF_INET;
        rtm.rtm_dst_len = (u8)mask_to_prefix(r->mask);
        rtm.rtm_table = RT_TABLE_MAIN;
        bool is_gw = (r->flags & RT_FLAG_GATEWAY) != 0;
        rtm.rtm_protocol = is_gw ? RTPROT_DHCP : RTPROT_KERNEL;
        rtm.rtm_scope = is_gw ? RT_SCOPE_UNIVERSE : RT_SCOPE_LINK;
        rtm.rtm_type = RTN_UNICAST;
        memcpy(buf + off, &rtm, sizeof(rtm));
        off += sizeof(rtm);

        bool has_dst = (r->dst[0] || r->dst[1] || r->dst[2] || r->dst[3]);
        if (has_dst) nl_put_attr(buf, &off, RTA_DST, r->dst, 4);
        if (is_gw) nl_put_attr(buf, &off, RTA_GATEWAY, r->gateway, 4);

        int oif = -1;
        if (r->dev) {
            int devcount = net_get_device_count();
            for (int k = 0; k < devcount; k++) {
                if (net_get_device_by_index(k) == r->dev) { oif = k; break; }
            }
        }
        if (oif >= 0) {
            u32 oifv = (u32)oif;
            nl_put_attr(buf, &off, RTA_OIF, &oifv, sizeof(oifv));
        }
        u32 prio = r->metric;
        nl_put_attr(buf, &off, RTA_PRIORITY, &prio, sizeof(prio));

        nl_end_msg(buf, hdr_off, &off);
    }

    size_t hdr_off;
    nl_begin_msg(buf, &off, NLMSG_DONE, NLM_F_MULTI, seq, pid, &hdr_off);
    nl_end_msg(buf, hdr_off, &off);
    return off;
}

s64 netlink_socket_send(netlink_sock_t *nl, const void *data, size_t len)
{
    if (!nl || !data || len < sizeof(nlmsghdr_t)) return -1;
    const nlmsghdr_t *req = (const nlmsghdr_t *)data;

    u8 *resp = (u8 *)kmalloc(NL_RESP_BUF_SIZE);
    if (!resp) return -1;
    size_t resp_len;

    switch (req->nlmsg_type) {
    case RTM_GETLINK:
        resp_len = nl_build_getlink(resp, NL_RESP_BUF_SIZE, req->nlmsg_seq, req->nlmsg_pid);
        break;
    case RTM_GETADDR:
        resp_len = nl_build_getaddr(resp, NL_RESP_BUF_SIZE, req->nlmsg_seq, req->nlmsg_pid);
        break;
    case RTM_GETROUTE:
        resp_len = nl_build_getroute(resp, NL_RESP_BUF_SIZE, req->nlmsg_seq, req->nlmsg_pid);
        break;
    default: {
        /* Every rtnetlink request gets *some* reply; a request kind nothing
         * above implements (RTM_NEWADDR, RTM_NEWROUTE, RTM_SETLINK, ...) —
         * modifying state via netlink isn't supported — gets an explicit
         * -EOPNOTSUPP instead of silent success or silence. */
        size_t off = 0, hdr_off;
        nl_begin_msg(resp, &off, NLMSG_ERROR, 0, req->nlmsg_seq, req->nlmsg_pid, &hdr_off);
        nlmsgerr_t err;
        memset(&err, 0, sizeof(err));
        err.error = -95; /* -EOPNOTSUPP */
        err.msg = *req;
        memcpy(resp + off, &err, sizeof(err));
        off += sizeof(err);
        nl_end_msg(resp, hdr_off, &off);
        resp_len = off;
        break;
    }
    }

    net_buf_t *buf = net_buf_alloc(resp_len);
    if (buf) {
        void *p = net_buf_put(buf, resp_len);
        memcpy(p, resp, resp_len);
        net_buf_queue_push(&nl->rx_queue, buf);

        irqflags_t flags = spinlock_lock_irqsave(&nl->lock);
        if (nl->wait_thread) {
            sched_unblock(nl->wait_thread);
            nl->wait_thread = NULL;
        }
        spinlock_unlock_irqrestore(&nl->lock, flags);
    }
    kfree(resp);
    return (s64)len;
}
