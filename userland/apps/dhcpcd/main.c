/* ============================================================================
 * AzamiOS Userspace — RFC 2131 DHCP Client Daemon (dhcpcd.elf)
 * File: userland/apps/dhcpcd/main.c
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <errno.h>

#define DHCP_CLIENT_PORT 68
#define DHCP_SERVER_PORT 67

#define DHCP_BOOTREQUEST 1
#define DHCP_BOOTREPLY   2
#define DHCP_HTYPE_ETH   1
#define DHCP_MAGIC_COOKIE 0x63825363

#define DHCP_OPT_PAD            0
#define DHCP_OPT_SUBNET_MASK    1
#define DHCP_OPT_ROUTER         3
#define DHCP_OPT_DNS            6
#define DHCP_OPT_HOSTNAME       12
#define DHCP_OPT_DOMAIN_NAME    15
#define DHCP_OPT_REQUESTED_IP   50
#define DHCP_OPT_LEASE_TIME     51
#define DHCP_OPT_MSG_TYPE       53
#define DHCP_OPT_SERVER_ID      54
#define DHCP_OPT_PARAM_REQ_LIST 55
#define DHCP_OPT_CLIENT_ID      61
#define DHCP_OPT_END            255

#define DHCPDISCOVER 1
#define DHCPOFFER    2
#define DHCPREQUEST  3
#define DHCPACK      5
#define DHCPNAK      6
#define DHCPRELEASE  7

typedef struct __attribute__((packed)) {
    unsigned char  op;
    unsigned char  htype;
    unsigned char  hlen;
    unsigned char  hops;
    unsigned int   xid;
    unsigned short secs;
    unsigned short flags;
    unsigned char  ciaddr[4];
    unsigned char  yiaddr[4];
    unsigned char  siaddr[4];
    unsigned char  giaddr[4];
    unsigned char  chaddr[16];
    unsigned char  sname[64];
    unsigned char  file[128];
    unsigned int   magic_cookie;
    unsigned char  options[308];
} dhcp_packet_t;

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    printf("dhcpcd: starting RFC 2131 Dynamic Host Configuration on net0...\n");

    int net_fd = open("/dev/net0", O_RDWR, 0);
    unsigned char host_mac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
    if (net_fd >= 0) {
        ioctl(net_fd, 0x8910 /* SIOCGIFHWADDR */, (unsigned long)host_mac);
    }

    int sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sockfd < 0) {
        perror("dhcpcd: socket");
        if (net_fd >= 0) close(net_fd);
        return 1;
    }

    int broadcast_enable = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_BROADCAST, &broadcast_enable, sizeof(broadcast_enable));
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &broadcast_enable, sizeof(broadcast_enable));

    struct sockaddr_in client_addr;
    memset(&client_addr, 0, sizeof(client_addr));
    client_addr.sin_family = AF_INET;
    client_addr.sin_port = htons(DHCP_CLIENT_PORT);
    client_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sockfd, (struct sockaddr *)&client_addr, sizeof(client_addr)) < 0) {
        perror("dhcpcd: bind port 68");
        close(sockfd);
        if (net_fd >= 0) close(net_fd);
        return 1;
    }

    struct sockaddr_in bcast_server;
    memset(&bcast_server, 0, sizeof(bcast_server));
    bcast_server.sin_family = AF_INET;
    bcast_server.sin_port = htons(DHCP_SERVER_PORT);
    bcast_server.sin_addr.s_addr = inet_addr("255.255.255.255");

    unsigned int xid = 0x55AA7711;

    /* 1. Send DHCPDISCOVER */
    dhcp_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.op = DHCP_BOOTREQUEST;
    pkt.htype = DHCP_HTYPE_ETH;
    pkt.hlen = 6;
    pkt.xid = htonl(xid);
    pkt.flags = htons(0x8000);
    memcpy(pkt.chaddr, host_mac, 6);
    pkt.magic_cookie = htonl(DHCP_MAGIC_COOKIE);

    unsigned char *opt = pkt.options;
    *opt++ = DHCP_OPT_MSG_TYPE; *opt++ = 1; *opt++ = DHCPDISCOVER;
    *opt++ = DHCP_OPT_PARAM_REQ_LIST; *opt++ = 4;
    *opt++ = DHCP_OPT_SUBNET_MASK; *opt++ = DHCP_OPT_ROUTER; *opt++ = DHCP_OPT_DNS; *opt++ = DHCP_OPT_DOMAIN_NAME;
    *opt++ = DHCP_OPT_CLIENT_ID; *opt++ = 7; *opt++ = 1; memcpy(opt, host_mac, 6); opt += 6;
    *opt++ = DHCP_OPT_END;

    size_t send_len = sizeof(dhcp_packet_t) - sizeof(pkt.options) + (size_t)(opt - pkt.options);
    printf("dhcpcd: broadcasting DHCPDISCOVER...\n");
    sendto(sockfd, &pkt, send_len, 0, (struct sockaddr *)&bcast_server, sizeof(bcast_server));

    /* 2. Receive DHCPOFFER */
    dhcp_packet_t resp;
    unsigned char offered_ip[4] = { 0, 0, 0, 0 };
    unsigned char netmask[4] = { 0, 0, 0, 0 };
    unsigned char router[4] = { 0, 0, 0, 0 };
    unsigned char dns[4] = { 0, 0, 0, 0 };
    unsigned char server_id[4] = { 0, 0, 0, 0 };
    unsigned int lease_time = 0;

    ssize_t n = recvfrom(sockfd, &resp, sizeof(resp), 0, NULL, NULL);
    if (n > 0 && ntohl(resp.magic_cookie) == DHCP_MAGIC_COOKIE) {
        memcpy(offered_ip, resp.yiaddr, 4);

        /* Parse options */
        const unsigned char *p = resp.options;
        const unsigned char *end = (const unsigned char *)&resp + n;
        while (p < end && *p != DHCP_OPT_END) {
            if (*p == DHCP_OPT_PAD) { p++; continue; }
            unsigned char code = *p++;
            if (p >= end) break;
            unsigned char len = *p++;
            if (p + len > end) break;

            if (code == DHCP_OPT_SUBNET_MASK && len >= 4) memcpy(netmask, p, 4);
            else if (code == DHCP_OPT_ROUTER && len >= 4) memcpy(router, p, 4);
            else if (code == DHCP_OPT_DNS && len >= 4) memcpy(dns, p, 4);
            else if (code == DHCP_OPT_SERVER_ID && len >= 4) memcpy(server_id, p, 4);
            else if (code == DHCP_OPT_LEASE_TIME && len >= 4) {
                unsigned int lt; memcpy(&lt, p, 4); lease_time = ntohl(lt);
            }
            p += len;
        }

        printf("dhcpcd: received DHCPOFFER of %u.%u.%u.%u from server %u.%u.%u.%u\n",
               offered_ip[0], offered_ip[1], offered_ip[2], offered_ip[3],
               server_id[0], server_id[1], server_id[2], server_id[3]);

        /* 3. Send DHCPREQUEST */
        memset(&pkt, 0, sizeof(pkt));
        pkt.op = DHCP_BOOTREQUEST;
        pkt.htype = DHCP_HTYPE_ETH;
        pkt.hlen = 6;
        pkt.xid = htonl(xid);
        pkt.flags = htons(0x8000);
        memcpy(pkt.chaddr, host_mac, 6);
        pkt.magic_cookie = htonl(DHCP_MAGIC_COOKIE);

        opt = pkt.options;
        *opt++ = DHCP_OPT_MSG_TYPE; *opt++ = 1; *opt++ = DHCPREQUEST;
        *opt++ = DHCP_OPT_REQUESTED_IP; *opt++ = 4; memcpy(opt, offered_ip, 4); opt += 4;
        *opt++ = DHCP_OPT_SERVER_ID; *opt++ = 4; memcpy(opt, server_id, 4); opt += 4;
        *opt++ = DHCP_OPT_PARAM_REQ_LIST; *opt++ = 4;
        *opt++ = DHCP_OPT_SUBNET_MASK; *opt++ = DHCP_OPT_ROUTER; *opt++ = DHCP_OPT_DNS; *opt++ = DHCP_OPT_DOMAIN_NAME;
        *opt++ = DHCP_OPT_CLIENT_ID; *opt++ = 7; *opt++ = 1; memcpy(opt, host_mac, 6); opt += 6;
        *opt++ = DHCP_OPT_END;

        send_len = sizeof(dhcp_packet_t) - sizeof(pkt.options) + (size_t)(opt - pkt.options);
        printf("dhcpcd: sending DHCPREQUEST for %u.%u.%u.%u...\n",
               offered_ip[0], offered_ip[1], offered_ip[2], offered_ip[3]);
        sendto(sockfd, &pkt, send_len, 0, (struct sockaddr *)&bcast_server, sizeof(bcast_server));

        /* 4. Receive DHCPACK */
        n = recvfrom(sockfd, &resp, sizeof(resp), 0, NULL, NULL);
    }

    close(sockfd);

    /* 5. Apply network configuration via IOCTLs if acquired */
    if (offered_ip[0] != 0 || offered_ip[1] != 0 || offered_ip[2] != 0 || offered_ip[3] != 0) {
        if (net_fd >= 0) {
            ioctl(net_fd, 0x8916 /* SIOCSIFADDR */, (unsigned long)offered_ip);
            ioctl(net_fd, 0x891c /* SIOCSIFNETMASK */, (unsigned long)netmask);
            ioctl(net_fd, 0x891e /* SIOCSIFGW */, (unsigned long)router);
            ioctl(net_fd, 0x8921 /* SIOCSIFDNS */, (unsigned long)dns);
            close(net_fd);
        }

        /* 6. Write /etc/resolv.conf */
        int resolv_fd = open("/etc/resolv.conf", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (resolv_fd >= 0) {
            char resolv_buf[256];
            int rlen = snprintf(resolv_buf, sizeof(resolv_buf),
                "# Generated by dhcpcd for AzamiOS\n"
                "nameserver %u.%u.%u.%u\n"
                "nameserver 8.8.8.8\n",
                dns[0], dns[1], dns[2], dns[3]
            );
            write(resolv_fd, resolv_buf, (size_t)rlen);
            close(resolv_fd);
        }

        printf("dhcpcd: lease BOUND successfully:\n");
        printf("        IP:         %u.%u.%u.%u\n", offered_ip[0], offered_ip[1], offered_ip[2], offered_ip[3]);
        printf("        Netmask:    %u.%u.%u.%u\n", netmask[0], netmask[1], netmask[2], netmask[3]);
        printf("        Gateway:    %u.%u.%u.%u\n", router[0], router[1], router[2], router[3]);
        printf("        DNS:        %u.%u.%u.%u\n", dns[0], dns[1], dns[2], dns[3]);
        printf("        Lease Time: %u seconds\n", lease_time);
    } else {
        if (net_fd >= 0) close(net_fd);
        printf("dhcpcd: no DHCP response received from network.\n");
    }

    return 0;
}
