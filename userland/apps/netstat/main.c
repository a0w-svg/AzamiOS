#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#ifndef SIOCGIFHWADDR
#define SIOCGIFHWADDR   0x8910
#endif
#ifndef SIOCGIFFLAGS
#define SIOCGIFFLAGS    0x8913
#endif
#ifndef SIOCGIFADDR
#define SIOCGIFADDR     0x8915
#endif
#ifndef SIOCGIFNETMASK
#define SIOCGIFNETMASK  0x891b
#endif
#ifndef SIOCGIFMTU
#define SIOCGIFMTU      0x8922
#endif
#ifndef SIOCGIFSTATS
#define SIOCGIFSTATS    0x8925
#endif

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

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    unsigned int mtu = 1500;
    net_stats_t stats;
    unsigned char ip[4] = {0};
    memset(&stats, 0, sizeof(stats));

    int fd = open("/dev/net0", O_RDWR, 0);
    if (fd >= 0) {
        ioctl(fd, SIOCGIFMTU, (unsigned long)&mtu);
        ioctl(fd, SIOCGIFSTATS, (unsigned long)&stats);
        ioctl(fd, SIOCGIFADDR, (unsigned long)ip);
        close(fd);
    }

    printf("Kernel Interface table\n");
    printf("%-8s %-6s %-10s %-8s %-10s %-8s %-6s\n",
           "Iface", "MTU", "RX-OK", "RX-ERR", "TX-OK", "TX-ERR", "Flg");
    printf("%-8s %-6u %-10llu %-8llu %-10llu %-8llu %-6s\n",
           "net0", mtu, stats.rx_packets, stats.rx_errors, stats.tx_packets, stats.tx_errors, "BMRU");
    printf("%-8s %-6d %-10d %-8d %-10d %-8d %-6s\n\n",
           "lo", 65536, 0, 0, 0, 0, "LRU");

    printf("Active Internet connections (only servers or established)\n");
    printf("%-6s %-6s %-6s %-22s %-22s %-12s\n",
           "Proto", "Recv-Q", "Send-Q", "Local Address", "Foreign Address", "State");

    char local_str[32];
    snprintf(local_str, sizeof(local_str), "%u.%u.%u.%u:68", ip[0], ip[1], ip[2], ip[3]);
    printf("%-6s %-6d %-6d %-22s %-22s %-12s\n",
           "udp", 0, 0, local_str, "0.0.0.0:*", "");
    printf("%-6s %-6d %-6d %-22s %-22s %-12s\n",
           "raw", 0, 0, "0.0.0.0:1 (ICMP)", "0.0.0.0:*", "");

    return 0;
}
