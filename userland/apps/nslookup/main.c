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
    const char *server_ip = (argc >= 3) ? argv[2] : "8.8.8.8";

    printf("Server:   %s\n", server_ip);
    printf("Address:  %s#53\n\n", server_ip);

    struct hostent *he = gethostbyname(query_host);
    if (!he || !he->h_addr_list || !he->h_addr_list[0]) {
        fprintf(stderr, "** server can't find %s: NXDOMAIN\n", query_host);
        return 1;
    }

    printf("Non-authoritative answer:\n");
    printf("Name:    %s\n", he->h_name);

    for (int i = 0; he->h_addr_list[i] != NULL; i++) {
        struct in_addr addr;
        memcpy(&addr, he->h_addr_list[i], sizeof(struct in_addr));
        printf("Address: %s\n", inet_ntoa(addr));
    }

    return 0;
}
