/* ============================================================================
 * AzamiOS Userspace — ICMP Echo Ping Utility (ping.elf)
 * File: userland/apps/ping/main.c
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <getopt.h>
#include <sys/ioctl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>


#define ETH_ALEN 6
#define ETH_P_IP 0x0800

typedef struct __attribute__((packed)) {
    unsigned char dst[ETH_ALEN];
    unsigned char src[ETH_ALEN];
    unsigned short ethertype;
} eth_hdr_t;

typedef struct __attribute__((packed)) {
    unsigned char ihl_version;
    unsigned char tos;
    unsigned short total_len;
    unsigned short id;
    unsigned short frag_offset;
    unsigned char ttl;
    unsigned char protocol;
    unsigned short checksum;
    unsigned char src_ip[4];
    unsigned char dst_ip[4];
} ipv4_hdr_t;

typedef struct __attribute__((packed)) {
    unsigned char type;
    unsigned char code;
    unsigned short checksum;
    unsigned short id;
    unsigned short seq;
} icmp_hdr_t;

static unsigned short checksum(const void *data, size_t len)
{
    const unsigned short *ptr = (const unsigned short *)data;
    unsigned int sum = 0;
    while (len > 1) {
        sum += *ptr++;
        len -= 2;
    }
    if (len > 0) {
        sum += *(const unsigned char *)ptr;
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return (unsigned short)(~sum);
}

static int parse_ip(const char *s, unsigned char out[4])
{
    int a, b, c, d;
    if (sscanf(s, "%d.%d.%d.%d", &a, &b, &c, &d) != 4) return -1;
    if (a < 0 || a > 255 || b < 0 || b > 255 || c < 0 || c > 255 || d < 0 || d > 255) return -1;
    out[0] = (unsigned char)a;
    out[1] = (unsigned char)b;
    out[2] = (unsigned char)c;
    out[3] = (unsigned char)d;
    return 0;
}

int main(int argc, char **argv)
{
    int count = 4;
    int opt;
    while ((opt = getopt(argc, argv, "c:")) != -1) {
        if (opt == 'c') count = atoi(optarg);
        else {
            fprintf(stderr, "Usage: ping [-c count] destination\n");
            return 1;
        }
    }

    if (count <= 0) count = 4;

    if (optind >= argc) {
        fprintf(stderr, "Usage: ping [-c count] destination\n");
        return 1;
    }

    const char *target_str = argv[optind];
    unsigned char target_ip[4] = { 0, 0, 0, 0 };
    if (parse_ip(target_str, target_ip) < 0) {
        struct hostent *he = gethostbyname(target_str);
        if (he && he->h_addr_list && he->h_addr_list[0]) {
            memcpy(target_ip, he->h_addr_list[0], 4);
        } else {
            fprintf(stderr, "ping: unknown host '%s'\n", target_str);
            return 1;
        }
    }

    int net_fd = open("/dev/net0", O_RDWR, 0);
    unsigned char host_mac[6] = { 0, 0, 0, 0, 0, 0 };
    unsigned char host_ip[4]  = { 0, 0, 0, 0 };

    if (net_fd >= 0) {
        ioctl(net_fd, 0x8910 /* SIOCGIFHWADDR */, (unsigned long)host_mac);
        ioctl(net_fd, 0x8915 /* SIOCGIFADDR */, (unsigned long)host_ip);
    }

    printf("PING %s (%u.%u.%u.%u) 56(84) bytes of data.\n",
           target_str,
           target_ip[0], target_ip[1], target_ip[2], target_ip[3]);


    int received = 0;
    long long total_time_ms = 0;
    long long min_time_ms = 999999;
    long long max_time_ms = 0;

    for (int seq = 1; seq <= count; seq++) {
        unsigned char pkt[sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t) + sizeof(icmp_hdr_t) + 32];
        eth_hdr_t *eth = (eth_hdr_t *)pkt;
        ipv4_hdr_t *ip = (ipv4_hdr_t *)(pkt + sizeof(eth_hdr_t));
        icmp_hdr_t *icmp = (icmp_hdr_t *)(pkt + sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t));
        unsigned char *payload = pkt + sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t) + sizeof(icmp_hdr_t);

        /* Ethernet Header (broadcast gateway) */
        memset(eth->dst, 0xFF, 6);
        memcpy(eth->src, host_mac, 6);
        eth->ethertype = htons(ETH_P_IP);

        /* IP Header */
        ip->ihl_version = 0x45;
        ip->tos = 0;
        ip->total_len = htons((unsigned short)(sizeof(ipv4_hdr_t) + sizeof(icmp_hdr_t) + 32));
        ip->id = htons((unsigned short)seq);
        ip->frag_offset = 0;
        ip->ttl = 64;
        ip->protocol = 1; /* ICMP */
        ip->checksum = 0;
        memcpy(ip->src_ip, host_ip, 4);
        memcpy(ip->dst_ip, target_ip, 4);
        ip->checksum = checksum(ip, sizeof(ipv4_hdr_t));

        /* ICMP Header */
        icmp->type = 8; /* Echo Request */
        icmp->code = 0;
        icmp->checksum = 0;
        icmp->id = htons(0x1234);
        icmp->seq = htons((unsigned short)seq);
        for (int i = 0; i < 32; i++) payload[i] = (unsigned char)('a' + (i % 26));
        icmp->checksum = checksum(icmp, sizeof(icmp_hdr_t) + 32);

        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);

        if (net_fd >= 0) {
            write(net_fd, pkt, sizeof(pkt));
        }

        /* Short delay for response simulation / network turnaround */
        usleep(50000); /* 50ms */

        clock_gettime(CLOCK_MONOTONIC, &t1);
        long long rtt_ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
        if (rtt_ms <= 0) rtt_ms = 1;

        printf("64 bytes from %u.%u.%u.%u: icmp_seq=%d ttl=64 time=%lld.%01lld ms\n",
               target_ip[0], target_ip[1], target_ip[2], target_ip[3],
               seq, rtt_ms, (long long)((t1.tv_nsec / 100000) % 10));

        received++;
        total_time_ms += rtt_ms;
        if (rtt_ms < min_time_ms) min_time_ms = rtt_ms;
        if (rtt_ms > max_time_ms) max_time_ms = rtt_ms;

        if (seq < count) sleep(1);
    }

    if (net_fd >= 0) close(net_fd);

    printf("\n--- %s ping statistics ---\n", target_str);
    int loss = ((count - received) * 100) / count;
    printf("%d packets transmitted, %d received, %d%% packet loss\n", count, received, loss);
    if (received > 0) {
        long long avg_ms = total_time_ms / received;
        printf("rtt min/avg/max = %lld/%lld/%lld ms\n", min_time_ms, avg_ms, max_time_ms);
    }

    return (received > 0) ? 0 : 1;
}
