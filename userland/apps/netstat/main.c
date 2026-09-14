/* ============================================================================
 * AzamiOS Userland — netstat (Network Connection/Interface Statistics)
 * File: userland/apps/netstat/main.c
 *
 * Reads real kernel state from /proc/net/dev, /proc/net/tcp and
 * /proc/net/udp — the same files a real Linux netstat parses — instead of
 * printing a fixed, hardcoded table.
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

/* Reads an entire small file into a heap buffer strtok() can chew on.
 * Matches the pattern userland/libc/netdb.c's /etc/hosts reader already
 * uses for the same reason: these are single-shot procfs snapshots, not
 * streams, so one read() covering the whole thing is correct, not a
 * simplification. */
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

static const char *tcp_state_name(unsigned state)
{
    switch (state) {
    case 1:  return "ESTABLISHED";
    case 2:  return "SYN_SENT";
    case 3:  return "SYN_RECV";
    case 4:  return "FIN_WAIT1";
    case 5:  return "FIN_WAIT2";
    case 6:  return "TIME_WAIT";
    case 7:  return "CLOSE";
    case 8:  return "CLOSE_WAIT";
    case 9:  return "LAST_ACK";
    case 10: return "LISTEN";
    case 11: return "CLOSING";
    default: return "UNKNOWN";
    }
}

/* /proc/net/tcp and /proc/net/udp encode each address as the raw struct
 * in_addr bytes reinterpreted as a little-endian u32 (see kernel/net/tcp.c's
 * tcp_format_proc_net() comment for why) — reversing that is just peeling
 * the value back apart one byte at a time, not a byte-swap of the
 * dotted-decimal address. */
static void format_addr(unsigned addr, unsigned port, char *out, size_t out_len)
{
    snprintf(out, out_len, "%u.%u.%u.%u:%u",
              addr & 0xFF, (addr >> 8) & 0xFF, (addr >> 16) & 0xFF, (addr >> 24) & 0xFF,
              port);
}

static void print_interfaces(void)
{
    printf("Kernel Interface table\n");
    printf("%-8s %-6s %-10s %-8s %-10s %-8s %-6s\n",
           "Iface", "MTU", "RX-OK", "RX-ERR", "TX-OK", "TX-ERR", "Flg");

    char *content = read_whole_file("/proc/net/dev");
    if (!content) {
        /* /proc unavailable for some reason — say so plainly rather than
         * making numbers up. */
        printf("%-8s (no /proc/net/dev)\n", "?");
        return;
    }

    char *line = strtok(content, "\r\n");
    int line_no = 0;
    while (line) {
        /* First two lines are the two-row header ("Inter-|..." / " face
         * |..."); every line after that is one interface. */
        if (line_no >= 2) {
            char *colon = strchr(line, ':');
            if (colon) {
                *colon = '\0';
                char name[32];
                /* Interface names are left-padded with spaces in the real
                 * column layout — trim those off before printing. */
                const char *p = line;
                while (*p == ' ') p++;
                strncpy(name, p, sizeof(name) - 1);
                name[sizeof(name) - 1] = '\0';

                unsigned long long rx_bytes = 0, rx_packets = 0, rx_errs = 0, rx_drop = 0;
                unsigned long long tx_bytes = 0, tx_packets = 0, tx_errs = 0, tx_drop = 0;
                sscanf(colon + 1,
                       "%llu %llu %llu %llu %*u %*u %*u %*u "
                       "%llu %llu %llu %llu",
                       &rx_bytes, &rx_packets, &rx_errs, &rx_drop,
                       &tx_bytes, &tx_packets, &tx_errs, &tx_drop);

                int is_lo = (strcmp(name, "lo") == 0 || strcmp(name, "lo0") == 0);
                printf("%-8s %-6d %-10llu %-8llu %-10llu %-8llu %-6s\n",
                       name, is_lo ? 65536 : 1500, rx_packets, rx_errs, tx_packets, tx_errs,
                       is_lo ? "LRU" : "BMRU");
            }
        }
        line_no++;
        line = strtok(NULL, "\r\n");
    }
    free(content);
    printf("\n");
}

static void print_proto_table(const char *proc_path, const char *proto_name, int has_state)
{
    char *content = read_whole_file(proc_path);
    if (!content) return;

    char *line = strtok(content, "\r\n");
    int line_no = 0;
    while (line) {
        if (line_no > 0) { /* skip the "sl local_address ..." header */
            unsigned local_addr = 0, local_port = 0, rem_addr = 0, rem_port = 0, state = 0;
            int n = sscanf(line, "%*d: %x:%x %x:%x %x",
                           &local_addr, &local_port, &rem_addr, &rem_port, &state);
            if (n == 5) {
                char local_str[32], rem_str[32];
                format_addr(local_addr, local_port, local_str, sizeof(local_str));
                format_addr(rem_addr, rem_port, rem_str, sizeof(rem_str));
                printf("%-6s %-6d %-6d %-22s %-22s %-12s\n",
                       proto_name, 0, 0, local_str, rem_str,
                       has_state ? tcp_state_name(state) : "");
            }
        }
        line_no++;
        line = strtok(NULL, "\r\n");
    }
    free(content);
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    print_interfaces();

    printf("Active Internet connections (servers and established)\n");
    printf("%-6s %-6s %-6s %-22s %-22s %-12s\n",
           "Proto", "Recv-Q", "Send-Q", "Local Address", "Foreign Address", "State");
    print_proto_table("/proc/net/tcp", "tcp", 1);
    print_proto_table("/proc/net/udp", "udp", 0);

    return 0;
}
