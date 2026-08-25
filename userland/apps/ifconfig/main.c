#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/string.h"
#include "../../libc/include/fcntl.h"
#include "../../libc/include/unistd.h"
#include "../../libc/include/sys/ioctl.h"

#define SIOCGIFHWADDR   0x8910
#define SIOCGIFFLAGS    0x8913
#define SIOCSIFFLAGS    0x8914
#define SIOCGIFADDR     0x8915
#define SIOCSIFADDR     0x8916
#define SIOCGIFNETMASK  0x891b
#define SIOCSIFNETMASK  0x891c
#define SIOCGIFBRDADDR  0x8919
#define SIOCSIFBRDADDR  0x891a
#define SIOCGIFGW       0x891d
#define SIOCSIFGW       0x891e
#define SIOCGIFDNS      0x891f
#define SIOCSIFDNS      0x8921
#define SIOCGIFMTU      0x8922
#define SIOCGIFSTATS    0x8925
#define SIOCSIFDHCP     0x8990
#define SIOCGIFDHCP     0x8991

typedef struct {
    unsigned long long rx_packets;
    unsigned long long tx_packets;
    unsigned long long rx_bytes;
    unsigned long long tx_bytes;
    unsigned long long rx_errors;
    unsigned long long tx_errors;
    unsigned long long rx_dropped;
    unsigned long long tx_dropped;
} net_stats_t;

static int parse_ipv4(const char *s, unsigned char out[4])
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
    int fd = open("/dev/net0", O_RDWR, 0);

    /* If arguments provided: set configuration */
    if (argc >= 2) {
        if (fd < 0) {
            fprintf(stderr, "ifconfig: cannot open /dev/net0\n");
            return 1;
        }

        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "dhcp") == 0) {
                printf("ifconfig: requesting DHCP lease configuration...\n");
                ioctl(fd, SIOCSIFDHCP, 0);
            } else if (strcmp(argv[i], "netmask") == 0 && i + 1 < argc) {
                unsigned char nm[4];
                if (parse_ipv4(argv[++i], nm) == 0) {
                    ioctl(fd, SIOCSIFNETMASK, (unsigned long)nm);
                } else {
                    fprintf(stderr, "ifconfig: invalid netmask '%s'\n", argv[i]);
                }
            } else if (strcmp(argv[i], "gw") == 0 || strcmp(argv[i], "gateway") == 0) {
                if (i + 1 < argc) {
                    unsigned char gw[4];
                    if (parse_ipv4(argv[++i], gw) == 0) {
                        ioctl(fd, SIOCSIFGW, (unsigned long)gw);
                    }
                }
            } else if (strcmp(argv[i], "up") == 0) {
                int flags = 0x4163;
                ioctl(fd, SIOCSIFFLAGS, (unsigned long)&flags);
            } else if (strcmp(argv[i], "down") == 0) {
                int flags = 0;
                ioctl(fd, SIOCSIFFLAGS, (unsigned long)&flags);
            } else if (strcmp(argv[i], "net0") != 0 && strcmp(argv[i], "lo") != 0) {
                unsigned char ip[4];
                if (parse_ipv4(argv[i], ip) == 0) {
                    ioctl(fd, SIOCSIFADDR, (unsigned long)ip);
                }
            }
        }
    }

    unsigned char mac[6] = { 0, 0, 0, 0, 0, 0 };
    unsigned char ip[4]  = { 0, 0, 0, 0 };
    unsigned char nm[4]  = { 0, 0, 0, 0 };
    unsigned char gw[4]  = { 0, 0, 0, 0 };
    unsigned char dns[4] = { 0, 0, 0, 0 };
    unsigned int  mtu    = 1500;
    net_stats_t   stats;
    memset(&stats, 0, sizeof(stats));

    if (fd >= 0) {
        ioctl(fd, SIOCGIFHWADDR, (unsigned long)mac);
        ioctl(fd, SIOCGIFADDR, (unsigned long)ip);
        ioctl(fd, SIOCGIFNETMASK, (unsigned long)nm);
        ioctl(fd, SIOCGIFGW, (unsigned long)gw);
        ioctl(fd, SIOCGIFDNS, (unsigned long)dns);
        ioctl(fd, SIOCGIFMTU, (unsigned long)&mtu);
        ioctl(fd, SIOCGIFSTATS, (unsigned long)&stats);
        close(fd);
    }

    unsigned char bcast[4] = {
        (unsigned char)(ip[0] | ~nm[0]),
        (unsigned char)(ip[1] | ~nm[1]),
        (unsigned char)(ip[2] | ~nm[2]),
        (unsigned char)(ip[3] | ~nm[3])
    };

    printf("net0: flags=4163<UP,BROADCAST,RUNNING,MULTICAST>  mtu %u\n", mtu);
    printf("        inet %u.%u.%u.%u  netmask %u.%u.%u.%u  broadcast %u.%u.%u.%u\n",
           ip[0], ip[1], ip[2], ip[3],
           nm[0], nm[1], nm[2], nm[3],
           bcast[0], bcast[1], bcast[2], bcast[3]);
    if (gw[0] || gw[1] || gw[2] || gw[3]) {
        printf("        gateway %u.%u.%u.%u  dns %u.%u.%u.%u\n",
               gw[0], gw[1], gw[2], gw[3],
               dns[0], dns[1], dns[2], dns[3]);
    }
    printf("        ether %02x:%02x:%02x:%02x:%02x:%02x  txqueuelen 1000  (Ethernet)\n",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    printf("        RX packets %llu  bytes %llu\n", stats.rx_packets, stats.rx_bytes);
    printf("        TX packets %llu  bytes %llu\n\n", stats.tx_packets, stats.tx_bytes);

    printf("lo: flags=73<UP,LOOPBACK,RUNNING>  mtu 65536\n");
    printf("        inet 127.0.0.1  netmask 255.0.0.0\n");
    printf("        loop  txqueuelen 1000  (Local Loopback)\n");

    return 0;
}
