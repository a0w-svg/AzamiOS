/* ============================================================================
 * AzamiOS Userland — Linux ip (IP Route/Address/Link Configuration) Utility
 * File: userland/apps/ip/main.c
 *
 * `link`/`addr` come from /sys/class/net (already live kernel state) plus
 * SIOCGIFADDR/SIOCGIFNETMASK/SIOCGIFBRDADDR/SIOCGIFMTU on /dev/net0 (this
 * kernel models one active interface's address state globally, so any
 * /dev/netN node answers the same ioctls the same way — see net_ioctl() in
 * kernel/net/net.c); `route` comes from /proc/net/route, the same file a
 * real Linux `ip route` ultimately reads via rtnetlink-equivalent data.
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#ifndef SIOCGIFADDR
#define SIOCGIFADDR     0x8915
#endif
#ifndef SIOCGIFNETMASK
#define SIOCGIFNETMASK  0x891b
#endif
#ifndef SIOCGIFBRDADDR
#define SIOCGIFBRDADDR  0x8919
#endif
#ifndef SIOCGIFMTU
#define SIOCGIFMTU      0x8922
#endif

static void read_net_attr(const char *dev, const char *attr, char *out, size_t max_len)
{
    char path[128];
    snprintf(path, sizeof(path), "/sys/class/net/%s/%s", dev, attr);
    int fd = open(path, O_RDONLY);
    if (fd >= 0) {
        ssize_t n = read(fd, out, max_len - 1);
        close(fd);
        if (n > 0) {
            out[n] = '\0';
            char *nl = strchr(out, '\n');
            if (nl) *nl = '\0';
            return;
        }
    }
    out[0] = '\0';
}

/* Same single-shot-snapshot reasoning as userland/libc/netdb.c's
 * /etc/hosts reader and userland/apps/netstat/main.c's identical helper. */
static char *read_whole_file(const char *path)
{
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return NULL;
    char *buf = (char *)malloc(16384);
    if (!buf) { close(fd); return NULL; }
    ssize_t n = read(fd, buf, 16383);
    close(fd);
    if (n < 0) { free(buf); return NULL; }
    buf[n] = '\0';
    return buf;
}

static int mask_to_prefix(const unsigned char mask[4])
{
    int bits = 0;
    for (int i = 0; i < 4; i++) {
        unsigned char m = mask[i];
        while (m) { bits += (m & 1); m >>= 1; }
    }
    return bits;
}

/* The primary (non-loopback) interface's real name, as /sys/class/net
 * actually registered it (the driver's own name — "e1000", "rtl8139", ... —
 * not a fabricated "eth0"; see fs/sysfs.c's SYSFS_TYPE_CLASS_NET_DIR). This
 * kernel models exactly one such interface, so the first non-"lo" entry
 * found is *the* interface. */
static void primary_iface_name(char *out, size_t out_len)
{
    strncpy(out, "eth0", out_len - 1);
    out[out_len - 1] = '\0';

    DIR *d = opendir("/sys/class/net");
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        if (strcmp(de->d_name, "lo") == 0) continue;
        strncpy(out, de->d_name, out_len - 1);
        out[out_len - 1] = '\0';
        break;
    }
    closedir(d);
}

static void show_ip_link(void)
{
    DIR *d = opendir("/sys/class/net");
    if (!d) {
        printf("1: lo: <LOOPBACK,UP,LOWER_UP> mtu 65536 qdisc noqueue state UNKNOWN\n");
        printf("    link/loopback 00:00:00:00:00:00 brd 00:00:00:00:00:00\n");
        return;
    }

    struct dirent *de;
    int idx = 1;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;

        char oper[32], mtu[16], mac[32];
        read_net_attr(de->d_name, "operstate", oper, sizeof(oper));
        read_net_attr(de->d_name, "mtu", mtu, sizeof(mtu));
        read_net_attr(de->d_name, "address", mac, sizeof(mac));

        if (oper[0] == '\0') strncpy(oper, "UP", sizeof(oper) - 1);
        if (mtu[0] == '\0') strncpy(mtu, "1500", sizeof(mtu) - 1);

        int is_lo = (strcmp(de->d_name, "lo") == 0);
        const char *flags = is_lo ? "LOOPBACK,UP,LOWER_UP" : "BROADCAST,MULTICAST,UP,LOWER_UP";
        const char *link_type = is_lo ? "link/loopback" : "link/ether";

        printf("%d: %s: <%s> mtu %s qdisc %s state %s\n",
               idx, de->d_name, flags, mtu, is_lo ? "noqueue" : "pfifo_fast", oper);
        printf("    %s %s brd %s\n",
               link_type, mac[0] ? mac : (is_lo ? "00:00:00:00:00:00" : "00:00:00:00:00:00"),
               is_lo ? "00:00:00:00:00:00" : "ff:ff:ff:ff:ff:ff");
        idx++;
    }
    closedir(d);
}

static void show_ip_addr(void)
{
    printf("1: lo: <LOOPBACK,UP,LOWER_UP> mtu 65536 qdisc noqueue state UNKNOWN\n");
    printf("    link/loopback 00:00:00:00:00:00 brd 00:00:00:00:00:00\n");
    printf("    inet 127.0.0.1/8 scope host lo\n");
    printf("       valid_lft forever preferred_lft forever\n");

    char iface[32];
    primary_iface_name(iface, sizeof(iface));

    char mac[32];
    read_net_attr(iface, "address", mac, sizeof(mac));

    unsigned char ip[4] = {0}, mask[4] = {0}, brd[4] = {0};
    unsigned mtu = 1500;
    int fd = open("/dev/net0", O_RDWR, 0);
    if (fd >= 0) {
        ioctl(fd, SIOCGIFADDR, (unsigned long)ip);
        ioctl(fd, SIOCGIFNETMASK, (unsigned long)mask);
        ioctl(fd, SIOCGIFBRDADDR, (unsigned long)brd);
        ioctl(fd, SIOCGIFMTU, (unsigned long)&mtu);
        close(fd);
    }

    int has_ip = (ip[0] || ip[1] || ip[2] || ip[3]);
    printf("2: %s: <BROADCAST,MULTICAST%s> mtu %u qdisc pfifo_fast state %s\n",
           iface, has_ip ? ",UP,LOWER_UP" : "", mtu, has_ip ? "UP" : "DOWN");
    printf("    link/ether %s brd ff:ff:ff:ff:ff:ff\n", mac[0] ? mac : "00:00:00:00:00:00");
    if (has_ip) {
        printf("    inet %u.%u.%u.%u/%d brd %u.%u.%u.%u scope global %s\n",
               ip[0], ip[1], ip[2], ip[3], mask_to_prefix(mask),
               brd[0], brd[1], brd[2], brd[3], iface);
        printf("       valid_lft forever preferred_lft forever\n");
    }
}

static void show_ip_route(void)
{
    char *content = read_whole_file("/proc/net/route");
    if (!content) return;

    unsigned char host_ip[4] = {0};
    int fd = open("/dev/net0", O_RDWR, 0);
    if (fd >= 0) {
        ioctl(fd, SIOCGIFADDR, (unsigned long)host_ip);
        close(fd);
    }

    char *line = strtok(content, "\r\n");
    int line_no = 0;
    while (line) {
        if (line_no > 0) { /* skip the "Iface Destination Gateway ..." header */
            char rt_iface[32];
            unsigned dest = 0, gw = 0, flags = 0, metric = 0, mask = 0;
            int n = sscanf(line, "%31s %x %x %x %*u %*u %u %x",
                           rt_iface, &dest, &gw, &flags, &metric, &mask);
            if (n == 6) {
                unsigned char d[4] = { (unsigned char)dest, (unsigned char)(dest >> 8),
                                        (unsigned char)(dest >> 16), (unsigned char)(dest >> 24) };
                unsigned char g[4] = { (unsigned char)gw, (unsigned char)(gw >> 8),
                                        (unsigned char)(gw >> 16), (unsigned char)(gw >> 24) };
                unsigned char m[4] = { (unsigned char)mask, (unsigned char)(mask >> 8),
                                        (unsigned char)(mask >> 16), (unsigned char)(mask >> 24) };
                int prefix = mask_to_prefix(m);
                int is_gw_route = (flags & 0x0002) != 0; /* RTF_GATEWAY */
                int is_default = (d[0] == 0 && d[1] == 0 && d[2] == 0 && d[3] == 0);

                if (is_default && is_gw_route) {
                    printf("default via %u.%u.%u.%u dev %s proto dhcp metric %u\n",
                           g[0], g[1], g[2], g[3], rt_iface, metric);
                } else if (d[0] == 127) {
                    printf("%u.%u.%u.%u/%d dev %s scope link\n",
                           d[0], d[1], d[2], d[3], prefix, rt_iface);
                } else {
                    printf("%u.%u.%u.%u/%d dev %s proto kernel scope link src %u.%u.%u.%u\n",
                           d[0], d[1], d[2], d[3], prefix, rt_iface,
                           host_ip[0], host_ip[1], host_ip[2], host_ip[3]);
                }
            }
        }
        line_no++;
        line = strtok(NULL, "\r\n");
    }
    free(content);
}

static void show_ip_addr_brief(void)
{
    char iface[32];
    primary_iface_name(iface, sizeof(iface));

    unsigned char ip[4] = {0}, mask[4] = {0};
    int fd = open("/dev/net0", O_RDWR, 0);
    if (fd >= 0) {
        ioctl(fd, SIOCGIFADDR, (unsigned long)ip);
        ioctl(fd, SIOCGIFNETMASK, (unsigned long)mask);
        close(fd);
    }
    int has_ip = (ip[0] || ip[1] || ip[2] || ip[3]);

    printf("%-16s %-14s %s\n", "lo", "UNKNOWN", "127.0.0.1/8");
    if (has_ip) {
        char cidr[32];
        snprintf(cidr, sizeof(cidr), "%u.%u.%u.%u/%d", ip[0], ip[1], ip[2], ip[3], mask_to_prefix(mask));
        printf("%-16s %-14s %s\n", iface, "UP", cidr);
    } else {
        printf("%-16s %-14s\n", iface, "DOWN");
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
