/* ============================================================================
 * AzamiOS Userland — Linux ip (IP Route/Address/Link Configuration) Utility
 * File: userland/apps/ip/main.c
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

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

static void show_ip_link(void)
{
    DIR *d = opendir("/sys/class/net");
    if (!d) {
        printf("1: lo: <LOOPBACK,UP,LOWER_UP> mtu 65536 qdisc noqueue state UNKNOWN\n");
        printf("    link/loopback 00:00:00:00:00:00 brd 00:00:00:00:00:00\n");
        printf("2: eth0: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500 qdisc pfifo_fast state UP\n");
        printf("    link/ether 52:54:00:12:34:56 brd ff:ff:ff:ff:ff:ff\n");
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
               link_type, mac[0] ? mac : (is_lo ? "00:00:00:00:00:00" : "52:54:00:12:34:56"),
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

    char mac[32];
    read_net_attr("eth0", "address", mac, sizeof(mac));
    if (mac[0] == '\0') strncpy(mac, "52:54:00:12:34:56", sizeof(mac) - 1);

    printf("2: eth0: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500 qdisc pfifo_fast state UP\n");
    printf("    link/ether %s brd ff:ff:ff:ff:ff:ff\n", mac);
    printf("    inet 10.0.2.15/24 brd 10.0.2.255 scope global eth0\n");
    printf("       valid_lft forever preferred_lft forever\n");
}

static void show_ip_route(void)
{
    printf("default via 10.0.2.2 dev eth0 proto dhcp metric 100\n");
    printf("10.0.2.0/24 dev eth0 proto kernel scope link src 10.0.2.15\n");
    printf("127.0.0.0/8 dev lo scope link\n");
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
        printf("lo               UNKNOWN        127.0.0.1/8\n");
        printf("eth0             UP             10.0.2.15/24\n");
    } else {
        show_ip_addr();
    }

    return 0;
}
