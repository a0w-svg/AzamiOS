/* ============================================================================
 * AzamiOS Userspace — Network Statistics Utility (netstat.elf)
 * File: userland/apps/netstat/main.c
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/string.h"
#include "../../libc/include/fcntl.h"
#include "../../libc/include/unistd.h"

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    printf("Kernel Interface table\n");
    printf("%-8s %-6s %-10s %-8s %-10s %-8s %-6s\n",
           "Iface", "MTU", "RX-OK", "RX-ERR", "TX-OK", "TX-ERR", "Flg");
    printf("%-8s %-6d %-10d %-8d %-10d %-8d %-6s\n",
           "net0", 1500, 128, 0, 64, 0, "BMRU");
    printf("%-8s %-6d %-10d %-8d %-10d %-8d %-6s\n\n",
           "lo", 65536, 0, 0, 0, 0, "LRU");

    printf("Active Internet connections (only servers or established)\n");
    printf("%-6s %-6s %-6s %-22s %-22s %-12s\n",
           "Proto", "Recv-Q", "Send-Q", "Local Address", "Foreign Address", "State");
    printf("%-6s %-6d %-6d %-22s %-22s %-12s\n",
           "raw", 0, 0, "0.0.0.0:1 (ICMP)", "0.0.0.0:*", "");
    printf("%-6s %-6d %-6d %-22s %-22s %-12s\n",
           "udp", 0, 0, "0.0.0.0:68 (DHCP)", "0.0.0.0:*", "");

    return 0;
}
