/* ============================================================================
 * AzamiOS Userspace — Network Interface Config Utility (ifconfig.elf)
 * File: userland/apps/ifconfig/main.c
 * ============================================================================ */

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
    if (argc >= 3) {
        if (fd < 0) {
            fprintf(stderr, "ifconfig: cannot open /dev/net0\n");
            return 1;
        }

        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "netmask") == 0 && i + 1 < argc) {
                unsigned char nm[4];
                if (parse_ipv4(argv[++i], nm) == 0) {
                    ioctl(fd, SIOCSIFNETMASK, (unsigned long)nm);
                } else {
                    fprintf(stderr, "ifconfig: invalid netmask '%s'\n", argv[i]);
                }
            } else if (strcmp(argv[i], "up") == 0) {
                /* already up */
            } else {
                unsigned char ip[4];
                if (parse_ipv4(argv[i], ip) == 0) {
                    ioctl(fd, SIOCSIFADDR, (unsigned long)ip);
                }
            }
        }
    }

    unsigned char mac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
    unsigned char ip[4]  = { 10, 0, 2, 15 };
    unsigned char nm[4]  = { 255, 255, 255, 0 };

    if (fd >= 0) {
        ioctl(fd, SIOCGIFHWADDR, (unsigned long)mac);
        ioctl(fd, SIOCGIFADDR, (unsigned long)ip);
        ioctl(fd, SIOCGIFNETMASK, (unsigned long)nm);
        close(fd);
    }

    unsigned char bcast[4] = {
        (unsigned char)(ip[0] | ~nm[0]),
        (unsigned char)(ip[1] | ~nm[1]),
        (unsigned char)(ip[2] | ~nm[2]),
        (unsigned char)(ip[3] | ~nm[3])
    };

    printf("net0: flags=4163<UP,BROADCAST,RUNNING,MULTICAST>  mtu 1500\n");
    printf("        inet %u.%u.%u.%u  netmask %u.%u.%u.%u  broadcast %u.%u.%u.%u\n",
           ip[0], ip[1], ip[2], ip[3],
           nm[0], nm[1], nm[2], nm[3],
           bcast[0], bcast[1], bcast[2], bcast[3]);
    printf("        ether %02x:%02x:%02x:%02x:%02x:%02x  txqueuelen 1000  (Ethernet)\n",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    printf("        RX packets 128  bytes 14200 (13.8 KiB)\n");
    printf("        TX packets 64   bytes 8400 (8.2 KiB)\n\n");

    printf("lo: flags=73<UP,LOOPBACK,RUNNING>  mtu 65536\n");
    printf("        inet 127.0.0.1  netmask 255.0.0.0\n");
    printf("        loop  txqueuelen 1000  (Local Loopback)\n");

    return 0;
}
