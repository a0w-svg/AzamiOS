/* ============================================================================
 * AzamiOS Userspace — ICMP Echo Ping Utility (ping.elf)
 * File: userland/apps/ping/main.c
 *
 * Sends ICMP echo requests and reports the replies that actually come back.
 *
 * This used to hand-build an Ethernet frame with a broadcast destination
 * MAC (no ARP), write it to /dev/net0, sleep 50ms, and then print
 * "64 bytes from <target>: icmp_seq=N ttl=64 time=50.0 ms" unconditionally
 * — it never read anything. Every host, reachable or not, came back as
 * 100% success with a 50ms round trip, which is exactly the number the
 * sleep had just burned.
 *
 * It now opens a raw ICMP socket and lets the kernel do the parts the
 * kernel owns: routing, ARP, the IPv4 header (kernel/net/socket.c's
 * raw_sock_t, ipv4_send(), and raw_input() on the way back). A reply
 * counts only if it is an echo reply carrying this process's identifier
 * and the sequence number just sent; anything else is a timeout, and the
 * statistics at the end are counts of real packets.
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <getopt.h>
#include <poll.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#ifndef IPPROTO_ICMP
#define IPPROTO_ICMP 1
#endif

#define ICMP_ECHO        8
#define ICMP_ECHOREPLY   0
#define ICMP_UNREACH     3
#define ICMP_TIMXCEED   11

#define PAYLOAD_LEN     56      /* the classic 56 data bytes -> 64-byte ICMP */

typedef struct __attribute__((packed)) {
    unsigned char  type;
    unsigned char  code;
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
    if (len > 0) sum += *(const unsigned char *)ptr;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (unsigned short)(~sum);
}

static long long now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

static int parse_ip(const char *s, unsigned char out[4])
{
    int a, b, c, d;
    if (sscanf(s, "%d.%d.%d.%d", &a, &b, &c, &d) != 4) return -1;
    if (a < 0 || a > 255 || b < 0 || b > 255 || c < 0 || c > 255 || d < 0 || d > 255) return -1;
    out[0] = (unsigned char)a; out[1] = (unsigned char)b;
    out[2] = (unsigned char)c; out[3] = (unsigned char)d;
    return 0;
}

/* Waits up to `deadline_us` for an echo reply matching (id, seq).
 *
 * A raw ICMP socket sees every ICMP packet the host receives, including
 * other processes' replies and unrelated errors, so each one is checked
 * before it is counted. The read gives the IP payload — the kernel strips
 * the IPv4 header on the way up (kernel/net/ipv4.c) — so the ICMP header
 * is at offset 0. Returns 1 on a match, 0 on timeout, -1 on error. */
static int await_reply(int fd, unsigned short id, unsigned short seq,
                       long long deadline_us)
{
    unsigned char buf[1500];

    for (;;) {
        long long remaining = deadline_us - now_us();
        if (remaining <= 0) return 0;

        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int pr = poll(&pfd, 1, (int)(remaining / 1000) + 1);
        if (pr < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (pr == 0) return 0;              /* timed out */

        ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            return -1;
        }
        if (n < (ssize_t)sizeof(icmp_hdr_t)) continue;

        const icmp_hdr_t *r = (const icmp_hdr_t *)buf;
        if (r->type != ICMP_ECHOREPLY) continue;          /* not a reply */
        if (ntohs(r->id) != id) continue;                 /* someone else's */
        if (ntohs(r->seq) != seq) continue;               /* an older one */

        return 1;
    }
}

int main(int argc, char **argv)
{
    int count = 4;
    int timeout_ms = 1000;
    int interval_ms = 1000;
    int opt;

    while ((opt = getopt(argc, argv, "c:W:i:")) != -1) {
        switch (opt) {
        case 'c': count = atoi(optarg); break;
        case 'W': timeout_ms = atoi(optarg); break;
        case 'i': interval_ms = atoi(optarg); break;
        default:
            fprintf(stderr, "Usage: ping [-c count] [-W timeout_ms] [-i interval_ms] destination\n");
            return 1;
        }
    }
    if (count <= 0) count = 4;
    if (timeout_ms <= 0) timeout_ms = 1000;
    if (interval_ms < 0) interval_ms = 0;

    if (optind >= argc) {
        fprintf(stderr, "Usage: ping [-c count] [-W timeout_ms] [-i interval_ms] destination\n");
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

    int fd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (fd < 0) {
        fprintf(stderr, "ping: cannot open raw ICMP socket: %s\n", strerror(errno));
        fprintf(stderr, "ping: (raw sockets are root-only)\n");
        return 1;
    }

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = 0;
    memcpy(&dst.sin_addr.s_addr, target_ip, 4);

    unsigned short id = (unsigned short)(getpid() & 0xFFFF);

    printf("PING %s (%u.%u.%u.%u) %d(%d) bytes of data.\n",
           target_str, target_ip[0], target_ip[1], target_ip[2], target_ip[3],
           PAYLOAD_LEN, PAYLOAD_LEN + 28);

    int transmitted = 0, received = 0;
    long long total_us = 0, min_us = 0, max_us = 0;

    for (int seq = 1; seq <= count; seq++) {
        unsigned char pkt[sizeof(icmp_hdr_t) + PAYLOAD_LEN];
        icmp_hdr_t *icmp = (icmp_hdr_t *)pkt;
        unsigned char *payload = pkt + sizeof(icmp_hdr_t);

        icmp->type = ICMP_ECHO;
        icmp->code = 0;
        icmp->checksum = 0;
        icmp->id  = htons(id);
        icmp->seq = htons((unsigned short)seq);
        for (int i = 0; i < PAYLOAD_LEN; i++) payload[i] = (unsigned char)('a' + (i % 26));
        icmp->checksum = checksum(pkt, sizeof(pkt));

        long long sent_us = now_us();
        ssize_t sret = sendto(fd, pkt, sizeof(pkt), 0,
                              (struct sockaddr *)&dst, sizeof(dst));
        if (sret < 0) {
            fprintf(stderr, "ping: send failed: %s\n", strerror(errno));
        } else {
            transmitted++;
        }

        int got = (sret < 0) ? 0
                : await_reply(fd, id, (unsigned short)seq,
                              sent_us + (long long)timeout_ms * 1000);

        if (got == 1) {
            long long rtt_us = now_us() - sent_us;
            received++;
            total_us += rtt_us;
            if (min_us == 0 || rtt_us < min_us) min_us = rtt_us;
            if (rtt_us > max_us) max_us = rtt_us;
            printf("%d bytes from %u.%u.%u.%u: icmp_seq=%d time=%lld.%03lld ms\n",
                   PAYLOAD_LEN + 8,
                   target_ip[0], target_ip[1], target_ip[2], target_ip[3],
                   seq, rtt_us / 1000, rtt_us % 1000);
        } else if (got == 0) {
            printf("Request timeout for icmp_seq %d\n", seq);
        } else {
            printf("ping: receive error on icmp_seq %d: %s\n", seq, strerror(errno));
        }

        if (seq < count && interval_ms > 0) {
            /* Sleep the remainder of the interval, not a flat second on top
             * of however long the reply took. */
            long long elapsed_us = now_us() - sent_us;
            long long left_us = (long long)interval_ms * 1000 - elapsed_us;
            if (left_us > 0) usleep((unsigned long)left_us);
        }
    }

    close(fd);

    printf("\n--- %s ping statistics ---\n", target_str);
    int loss = transmitted ? ((transmitted - received) * 100) / transmitted : 100;
    printf("%d packets transmitted, %d received, %d%% packet loss\n",
           transmitted, received, loss);
    if (received > 0) {
        long long avg_us = total_us / received;
        printf("rtt min/avg/max = %lld.%03lld/%lld.%03lld/%lld.%03lld ms\n",
               min_us / 1000, min_us % 1000,
               avg_us / 1000, avg_us % 1000,
               max_us / 1000, max_us % 1000);
    }

    return (received > 0) ? 0 : 1;
}
