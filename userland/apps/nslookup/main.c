/* ============================================================================
 * AzamiOS Userspace — DNS Name Resolution Utility (nslookup.elf)
 * File: userland/apps/nslookup/main.c
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: nslookup <hostname> [server]\n");
        return 1;
    }

    const char *query_host = argv[1];
    const char *server_ip;
    char discovered_server[64];

    if (argc >= 3) {
        server_ip = argv[2];
    } else if (res_get_nameserver(discovered_server, sizeof(discovered_server)) == 0) {
        server_ip = discovered_server;
    } else {
        fprintf(stderr, "*** No default name servers are configured\n");
        return 1;
    }

    printf("Server:   %s\n", server_ip);
    printf("Address:  %s#53\n\n", server_ip);

    struct in_addr addr;
    if (res_resolve_via(query_host, server_ip, &addr) != 0) {
        fprintf(stderr, "** server can't find %s: NXDOMAIN\n", query_host);
        return 1;
    }

    printf("Non-authoritative answer:\n");
    printf("Name:    %s\n", query_host);
    printf("Address: %s\n", inet_ntoa(addr));

    return 0;
}
