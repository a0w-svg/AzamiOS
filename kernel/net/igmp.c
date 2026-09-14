/* ============================================================================
 * AzamiOS — Internet Group Management Protocol v2 Engine (igmp.c)
 * File: kernel/net/igmp.c
 *
 * Implements RFC 2236 IGMPv2 host-side group membership.
 * ============================================================================ */

#define DEBUG 1
#include "../../include/azami/debug.h"
#include "../../include/azami/defs.h"
#include "../../include/azami/net.h"
#include "../../include/azami/ipv4.h"
#include "../../include/azami/igmp.h"
#include "../../arch/x86_64/cpu/spinlock.h"
#include "../../kernel/lib/string.h"

typedef struct {
    bool in_use;
    u8   group[4];
    int  refcount;
} igmp_group_t;

static igmp_group_t g_groups[IGMP_MAX_GROUPS];
static spinlock_t   g_igmp_lock = SPINLOCK_INIT;

void igmp_init(void)
{
    irqflags_t flags = spinlock_lock_irqsave(&g_igmp_lock);
    memset(g_groups, 0, sizeof(g_groups));
    spinlock_unlock_irqrestore(&g_igmp_lock, flags);
    pr_debug("[IGMP] IGMPv2 host membership engine initialized.\n");
}

static const u8 IGMP_ALL_ROUTERS[4] = { 224, 0, 0, 2 };

/*
 * A Report is sent to the group itself (every member, including the
 * sender, is listening on it); a Leave goes to All-Routers so whatever is
 * routing this segment notices even though no other host need care.
 * ipv4_send() already knows how to frame a 224.0.0.0/4 destination
 * directly onto the matching 01:00:5e:xx:xx:xx Ethernet address without
 * ARP — see ipv4_deliver() in ipv4.c.
 */
static void igmp_send(u8 type, const u8 group[4])
{
    net_buf_t *buf = net_buf_alloc(NET_BUF_HEADROOM + sizeof(igmp_hdr_t));
    if (!buf) return;

    net_buf_reserve(buf, NET_BUF_HEADROOM);
    igmp_hdr_t *h = (igmp_hdr_t *)net_buf_put(buf, sizeof(igmp_hdr_t));
    h->type = type;
    h->max_resp_time = 0;
    h->checksum = 0;
    memcpy(h->group, group, 4);
    h->checksum = net_checksum(h, sizeof(igmp_hdr_t));

    const u8 *dst = (type == IGMP_TYPE_V2_LEAVE) ? IGMP_ALL_ROUTERS : group;
    ipv4_send(buf, dst, IP_PROTO_IGMP);
}

int ip_multicast_join(const u8 group[4])
{
    if (!group) return -1;

    irqflags_t flags = spinlock_lock_irqsave(&g_igmp_lock);
    int free_slot = -1;
    for (int i = 0; i < IGMP_MAX_GROUPS; i++) {
        if (g_groups[i].in_use) {
            if (memcmp(g_groups[i].group, group, 4) == 0) {
                g_groups[i].refcount++;
                spinlock_unlock_irqrestore(&g_igmp_lock, flags);
                return 0; /* already a member; no new Report needed */
            }
        } else if (free_slot < 0) {
            free_slot = i;
        }
    }

    if (free_slot < 0) {
        spinlock_unlock_irqrestore(&g_igmp_lock, flags);
        return -1;
    }

    g_groups[free_slot].in_use = true;
    memcpy(g_groups[free_slot].group, group, 4);
    g_groups[free_slot].refcount = 1;
    spinlock_unlock_irqrestore(&g_igmp_lock, flags);

    igmp_send(IGMP_TYPE_V2_REPORT, group);
    return 0;
}

int ip_multicast_leave(const u8 group[4])
{
    if (!group) return -1;

    irqflags_t flags = spinlock_lock_irqsave(&g_igmp_lock);
    for (int i = 0; i < IGMP_MAX_GROUPS; i++) {
        if (g_groups[i].in_use && memcmp(g_groups[i].group, group, 4) == 0) {
            g_groups[i].refcount--;
            bool last = (g_groups[i].refcount <= 0);
            if (last) g_groups[i].in_use = false;
            spinlock_unlock_irqrestore(&g_igmp_lock, flags);
            if (last) igmp_send(IGMP_TYPE_V2_LEAVE, group);
            return 0;
        }
    }
    spinlock_unlock_irqrestore(&g_igmp_lock, flags);
    return -1;
}

bool ip_multicast_is_member(const u8 group[4])
{
    if (!group) return false;
    irqflags_t flags = spinlock_lock_irqsave(&g_igmp_lock);
    bool found = false;
    for (int i = 0; i < IGMP_MAX_GROUPS; i++) {
        if (g_groups[i].in_use && memcmp(g_groups[i].group, group, 4) == 0) {
            found = true;
            break;
        }
    }
    spinlock_unlock_irqrestore(&g_igmp_lock, flags);
    return found;
}

void igmp_input(net_buf_t *buf, const ipv4_hdr_t *ip_hdr)
{
    (void)ip_hdr;
    if (!buf) return;
    if (buf->len < sizeof(igmp_hdr_t)) {
        net_buf_free(buf);
        return;
    }

    const igmp_hdr_t *h = (const igmp_hdr_t *)buf->data;
    if (net_checksum(h, sizeof(igmp_hdr_t)) != 0) {
        pr_debug("[IGMP] Bad checksum, dropping.\n");
        net_buf_free(buf);
        return;
    }

    if (h->type == IGMP_TYPE_QUERY) {
        /*
         * A General Query (group 0.0.0.0) asks about every group we've
         * joined; a Group-Specific Query asks about one. Real IGMPv2 staggers
         * each host's Report over a random delay up to max_resp_time so one
         * report can suppress the others sharing a segment — pointless
         * complexity for a single-host stack with nothing else to suppress,
         * so this answers immediately instead.
         */
        u8 qgroup[4];
        memcpy(qgroup, h->group, 4);
        bool general = (qgroup[0] == 0 && qgroup[1] == 0 && qgroup[2] == 0 && qgroup[3] == 0);

        u8 snapshot[IGMP_MAX_GROUPS][4];
        int n = 0;
        irqflags_t flags = spinlock_lock_irqsave(&g_igmp_lock);
        for (int i = 0; i < IGMP_MAX_GROUPS; i++) {
            if (g_groups[i].in_use &&
                (general || memcmp(g_groups[i].group, qgroup, 4) == 0)) {
                memcpy(snapshot[n++], g_groups[i].group, 4);
            }
        }
        spinlock_unlock_irqrestore(&g_igmp_lock, flags);

        for (int i = 0; i < n; i++) {
            igmp_send(IGMP_TYPE_V2_REPORT, snapshot[i]);
        }
    }
    /* v1/v2 Reports and Leaves from other hosts: nothing for a pure host
     * (never a Querier) to do with them. */

    net_buf_free(buf);
}
