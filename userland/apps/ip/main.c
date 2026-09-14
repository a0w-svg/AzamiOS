/* ============================================================================
 * AzamiOS Userland — Linux ip (IP Route/Address/Link Configuration) Utility
 * File: userland/apps/ip/main.c
 *
 * Queries kernel state the real way: AF_NETLINK/NETLINK_ROUTE
 * (RTM_GETLINK/RTM_GETADDR/RTM_GETROUTE), the same protocol a genuine
 * iproute2 `ip` uses — see kernel/net/netlink.c for what actually answers
 * these on the other end. A single recv() gets the whole dump: that
 * kernel builds and queues one complete reply (every RTM_NEW* message plus
 * the NLMSG_DONE terminator) per request rather than streaming it, so
 * there's no need to loop recv() calls waiting for more.
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>

/* Matches include/azami/net.h's IFF_* bit values — this kernel's ifi_flags
 * carry net_device_t->flags verbatim (see kernel/net/netlink.c's
 * nl_build_getlink()). */
#define IFF_UP       0x0001
#define IFF_LOOPBACK 0x0008

#define NL_RESP_BUF_SIZE 16384
#define MAX_IFACES 8

typedef struct {
    int used;
    char name[32];
    unsigned char mac[6];
    unsigned int mtu;
    unsigned int flags;
    int have_addr;
    unsigned char addr[4];
    unsigned char brd[4];
    int prefixlen;
} iface_t;

static ssize_t nl_do_request(int rtm_type, void *buf, size_t bufsize)
{
    int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    if (fd < 0) return -1;

    struct sockaddr_nl sa;
    memset(&sa, 0, sizeof(sa));
    sa.nl_family = AF_NETLINK;
    bind(fd, (struct sockaddr *)&sa, sizeof(sa));

    struct {
        struct nlmsghdr nlh;
        unsigned char rtgen_family;
        unsigned char pad[3];
    } req;
    memset(&req, 0, sizeof(req));
    req.nlh.nlmsg_len = sizeof(req);
    req.nlh.nlmsg_type = (unsigned short)rtm_type;
    req.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    req.nlh.nlmsg_seq = 1;

    if (send(fd, &req, sizeof(req), 0) < 0) {
        close(fd);
        return -1;
    }

    ssize_t n = recv(fd, buf, bufsize, 0);
    close(fd);
    return n;
}

static struct rtattr *rta_first(void *msg_payload, size_t fixed_hdr_size, int msg_len, int *rlen_out)
{
    *rlen_out = msg_len - (int)NLMSG_ALIGN(fixed_hdr_size);
    return (struct rtattr *)((char *)msg_payload + NLMSG_ALIGN(fixed_hdr_size));
}

/* Collects link (name/mac/mtu/flags) and address (ip/brd/prefixlen) info
 * for every interface into one table, indexed by ifi_index/ifa_index —
 * what both `ip addr` and `ip route` (for RTA_OIF -> name) need. */
static int collect_ifaces(iface_t ifaces[MAX_IFACES])
{
    memset(ifaces, 0, sizeof(iface_t) * MAX_IFACES);

    static char linkbuf[NL_RESP_BUF_SIZE];
    ssize_t ln = nl_do_request(RTM_GETLINK, linkbuf, sizeof(linkbuf));
    if (ln > 0) {
        struct nlmsghdr *nlh = (struct nlmsghdr *)linkbuf;
        int len = (int)ln;
        while (NLMSG_OK(nlh, len)) {
            if (nlh->nlmsg_type == NLMSG_DONE) break;
            if (nlh->nlmsg_type == RTM_NEWLINK) {
                struct ifinfomsg *ifi = (struct ifinfomsg *)NLMSG_DATA(nlh);
                if (ifi->ifi_index >= 0 && ifi->ifi_index < MAX_IFACES) {
                    iface_t *f = &ifaces[ifi->ifi_index];
                    f->used = 1;
                    f->flags = ifi->ifi_flags;

                    int rlen;
                    struct rtattr *rta = rta_first(ifi, sizeof(*ifi),
                                                    (int)nlh->nlmsg_len - NLMSG_HDRLEN, &rlen);
                    for (struct rtattr *a = rta; RTA_OK(a, rlen); a = RTA_NEXT(a, rlen)) {
                        if (a->rta_type == IFLA_IFNAME) {
                            strncpy(f->name, (const char *)RTA_DATA(a), sizeof(f->name) - 1);
                        } else if (a->rta_type == IFLA_ADDRESS && RTA_PAYLOAD(a) >= 6) {
                            memcpy(f->mac, RTA_DATA(a), 6);
                        } else if (a->rta_type == IFLA_MTU && RTA_PAYLOAD(a) >= (int)sizeof(unsigned int)) {
                            memcpy(&f->mtu, RTA_DATA(a), sizeof(unsigned int));
                        }
                    }
                }
            }
            nlh = NLMSG_NEXT(nlh, len);
        }
    }

    static char addrbuf[NL_RESP_BUF_SIZE];
    ssize_t an = nl_do_request(RTM_GETADDR, addrbuf, sizeof(addrbuf));
    if (an > 0) {
        struct nlmsghdr *nlh = (struct nlmsghdr *)addrbuf;
        int len = (int)an;
        while (NLMSG_OK(nlh, len)) {
            if (nlh->nlmsg_type == NLMSG_DONE) break;
            if (nlh->nlmsg_type == RTM_NEWADDR) {
                struct ifaddrmsg *ifa = (struct ifaddrmsg *)NLMSG_DATA(nlh);
                if (ifa->ifa_index >= 0 && ifa->ifa_index < MAX_IFACES) {
                    iface_t *f = &ifaces[ifa->ifa_index];
                    f->used = 1;
                    f->prefixlen = ifa->ifa_prefixlen;

                    int rlen;
                    struct rtattr *rta = rta_first(ifa, sizeof(*ifa),
                                                    (int)nlh->nlmsg_len - NLMSG_HDRLEN, &rlen);
                    for (struct rtattr *a = rta; RTA_OK(a, rlen); a = RTA_NEXT(a, rlen)) {
                        if ((a->rta_type == IFA_LOCAL || a->rta_type == IFA_ADDRESS) && RTA_PAYLOAD(a) >= 4) {
                            memcpy(f->addr, RTA_DATA(a), 4);
                            f->have_addr = 1;
                        } else if (a->rta_type == IFA_BROADCAST && RTA_PAYLOAD(a) >= 4) {
                            memcpy(f->brd, RTA_DATA(a), 4);
                        } else if (a->rta_type == IFA_LABEL && f->name[0] == '\0') {
                            strncpy(f->name, (const char *)RTA_DATA(a), sizeof(f->name) - 1);
                        }
                    }
                }
            }
            nlh = NLMSG_NEXT(nlh, len);
        }
    }

    return MAX_IFACES;
}

static void show_ip_link(void)
{
    iface_t ifaces[MAX_IFACES];
    collect_ifaces(ifaces);

    for (int i = 0; i < MAX_IFACES; i++) {
        if (!ifaces[i].used) continue;
        iface_t *f = &ifaces[i];
        int is_lo = (f->flags & IFF_LOOPBACK) != 0;

        printf("%d: %s: <%s> mtu %u qdisc %s state %s\n",
               i, f->name[0] ? f->name : "?",
               is_lo ? "LOOPBACK,UP,LOWER_UP" : "BROADCAST,MULTICAST,UP,LOWER_UP",
               f->mtu, is_lo ? "noqueue" : "pfifo_fast",
               (f->flags & IFF_UP) ? "UP" : "DOWN");
        printf("    link/%s %02x:%02x:%02x:%02x:%02x:%02x brd %s\n",
               is_lo ? "loopback" : "ether",
               f->mac[0], f->mac[1], f->mac[2], f->mac[3], f->mac[4], f->mac[5],
               is_lo ? "00:00:00:00:00:00" : "ff:ff:ff:ff:ff:ff");
    }
}

static void show_ip_addr(void)
{
    iface_t ifaces[MAX_IFACES];
    collect_ifaces(ifaces);

    for (int i = 0; i < MAX_IFACES; i++) {
        if (!ifaces[i].used) continue;
        iface_t *f = &ifaces[i];
        int is_lo = (f->flags & IFF_LOOPBACK) != 0;

        printf("%d: %s: <%s> mtu %u qdisc %s state %s\n",
               i, f->name[0] ? f->name : "?",
               is_lo ? "LOOPBACK,UP,LOWER_UP" : "BROADCAST,MULTICAST,UP,LOWER_UP",
               f->mtu, is_lo ? "noqueue" : "pfifo_fast",
               (f->flags & IFF_UP) ? "UP" : "DOWN");
        printf("    link/%s %02x:%02x:%02x:%02x:%02x:%02x brd %s\n",
               is_lo ? "loopback" : "ether",
               f->mac[0], f->mac[1], f->mac[2], f->mac[3], f->mac[4], f->mac[5],
               is_lo ? "00:00:00:00:00:00" : "ff:ff:ff:ff:ff:ff");

        if (f->have_addr) {
            printf("    inet %u.%u.%u.%u/%d brd %u.%u.%u.%u scope %s %s\n",
                   f->addr[0], f->addr[1], f->addr[2], f->addr[3], f->prefixlen,
                   f->brd[0], f->brd[1], f->brd[2], f->brd[3],
                   is_lo ? "host" : "global", f->name);
            printf("       valid_lft forever preferred_lft forever\n");
        }
    }
}

static void show_ip_addr_brief(void)
{
    iface_t ifaces[MAX_IFACES];
    collect_ifaces(ifaces);

    for (int i = 0; i < MAX_IFACES; i++) {
        if (!ifaces[i].used) continue;
        iface_t *f = &ifaces[i];
        const char *state = (f->flags & IFF_UP) ? "UP" : "DOWN";
        if (f->flags & IFF_LOOPBACK) state = "UNKNOWN";

        if (f->have_addr) {
            printf("%-16s %-14s %u.%u.%u.%u/%d\n", f->name, state,
                   f->addr[0], f->addr[1], f->addr[2], f->addr[3], f->prefixlen);
        } else {
            printf("%-16s %-14s\n", f->name, state);
        }
    }
}

static void show_ip_route(void)
{
    iface_t ifaces[MAX_IFACES];
    collect_ifaces(ifaces);

    static char buf[NL_RESP_BUF_SIZE];
    ssize_t n = nl_do_request(RTM_GETROUTE, buf, sizeof(buf));
    if (n <= 0) return;

    struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
    int len = (int)n;
    while (NLMSG_OK(nlh, len)) {
        if (nlh->nlmsg_type == NLMSG_DONE) break;
        if (nlh->nlmsg_type == RTM_NEWROUTE) {
            struct rtmsg *rtm = (struct rtmsg *)NLMSG_DATA(nlh);
            unsigned char *dst = NULL, *gw = NULL;
            unsigned int oif = 0, have_oif = 0, metric = 0;

            int rlen;
            struct rtattr *rta = rta_first(rtm, sizeof(*rtm),
                                            (int)nlh->nlmsg_len - NLMSG_HDRLEN, &rlen);
            for (struct rtattr *a = rta; RTA_OK(a, rlen); a = RTA_NEXT(a, rlen)) {
                if (a->rta_type == RTA_DST && RTA_PAYLOAD(a) >= 4) dst = (unsigned char *)RTA_DATA(a);
                else if (a->rta_type == RTA_GATEWAY && RTA_PAYLOAD(a) >= 4) gw = (unsigned char *)RTA_DATA(a);
                else if (a->rta_type == RTA_OIF && RTA_PAYLOAD(a) >= (int)sizeof(unsigned int)) {
                    memcpy(&oif, RTA_DATA(a), sizeof(oif));
                    have_oif = 1;
                } else if (a->rta_type == RTA_PRIORITY && RTA_PAYLOAD(a) >= (int)sizeof(unsigned int)) {
                    memcpy(&metric, RTA_DATA(a), sizeof(metric));
                }
            }

            const char *devname = (have_oif && oif < MAX_IFACES && ifaces[oif].used)
                                       ? ifaces[oif].name : "?";

            if (!dst && gw) {
                printf("default via %u.%u.%u.%u dev %s proto dhcp metric %u\n",
                       gw[0], gw[1], gw[2], gw[3], devname, metric);
            } else if (dst) {
                if (dst[0] == 127) {
                    printf("%u.%u.%u.%u/%d dev %s scope link\n",
                           dst[0], dst[1], dst[2], dst[3], rtm->rtm_dst_len, devname);
                } else {
                    unsigned char *src = (have_oif && oif < MAX_IFACES) ? ifaces[oif].addr : NULL;
                    if (src) {
                        printf("%u.%u.%u.%u/%d dev %s proto kernel scope link src %u.%u.%u.%u\n",
                               dst[0], dst[1], dst[2], dst[3], rtm->rtm_dst_len, devname,
                               src[0], src[1], src[2], src[3]);
                    } else {
                        printf("%u.%u.%u.%u/%d dev %s proto kernel scope link\n",
                               dst[0], dst[1], dst[2], dst[3], rtm->rtm_dst_len, devname);
                    }
                }
            }
        }
        nlh = NLMSG_NEXT(nlh, len);
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        show_ip_addr();
        return 0;
    }

    const char *cmd = argv[1];
    if (strcmp(cmd, "link") == 0 || strcmp(cmd, "l") == 0) {
        show_ip_link();
    } else if (strcmp(cmd, "addr") == 0 || strcmp(cmd, "address") == 0 || strcmp(cmd, "a") == 0) {
        show_ip_addr();
    } else if (strcmp(cmd, "route") == 0 || strcmp(cmd, "r") == 0) {
        show_ip_route();
    } else if (strcmp(cmd, "-br") == 0 && argc >= 3 && (strcmp(argv[2], "a") == 0 || strcmp(argv[2], "addr") == 0)) {
        show_ip_addr_brief();
    } else {
        show_ip_addr();
    }

    return 0;
}
