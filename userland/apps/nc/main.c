/* ============================================================================
 * AzamiOS Userspace — Netcat Networking Utility (nc.elf)
 * File: userland/apps/nc/main.c
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>

static void usage(void)
{
    fprintf(stderr, "Usage: nc [-l] [-u] [-p port] [hostname] [port]\n");
    fprintf(stderr, "  -l        Listen mode, for inbound connects\n");
    fprintf(stderr, "  -u        UDP mode (default is TCP)\n");
    fprintf(stderr, "  -p port   Local port number to bind/listen\n");
    exit(1);
}

int main(int argc, char **argv)
{
    int listen_mode = 0;
    int udp_mode = 0;
    int local_port = 0;
    const char *host_str = NULL;
    int target_port = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-l") == 0) {
            listen_mode = 1;
        } else if (strcmp(argv[i], "-u") == 0) {
            udp_mode = 1;
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            local_port = atoi(argv[++i]);
        } else if (!host_str && !listen_mode) {
            host_str = argv[i];
        } else if (!target_port) {
            target_port = atoi(argv[i]);
        }
    }

    if (listen_mode && local_port == 0 && target_port > 0) {
        local_port = target_port;
    }

    if (!listen_mode && (!host_str || target_port == 0)) {
        usage();
    }

    int sock_type = udp_mode ? SOCK_DGRAM : SOCK_STREAM;
    int proto = udp_mode ? IPPROTO_UDP : IPPROTO_TCP;

    int sockfd = socket(AF_INET, sock_type, proto);
    if (sockfd < 0) {
        perror("nc: socket");
        return 1;
    }

    int opt = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    int active_fd = sockfd;

    if (listen_mode) {
        struct sockaddr_in bind_addr;
        memset(&bind_addr, 0, sizeof(bind_addr));
        bind_addr.sin_family = AF_INET;
        bind_addr.sin_port = htons((unsigned short)local_port);
        bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);

        if (bind(sockfd, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
            perror("nc: bind");
            close(sockfd);
            return 1;
        }

        if (!udp_mode) {
            if (listen(sockfd, 1) < 0) {
                perror("nc: listen");
                close(sockfd);
                return 1;
            }

            printf("nc: listening on port %d...\n", local_port);
            struct sockaddr_in client_addr;
            socklen_t addrlen = sizeof(client_addr);
            active_fd = accept(sockfd, (struct sockaddr *)&client_addr, &addrlen);
            if (active_fd < 0) {
                perror("nc: accept");
                close(sockfd);
                return 1;
            }
            printf("nc: connection accepted from %s:%u\n",
                   inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
        } else {
            printf("nc: listening for UDP datagrams on port %d...\n", local_port);
        }
    } else {
        struct addrinfo hints, *res;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = sock_type;

        char port_str[16];
        snprintf(port_str, sizeof(port_str), "%d", target_port);

        int err = getaddrinfo(host_str, port_str, &hints, &res);
        if (err != 0 || !res) {
            fprintf(stderr, "nc: getaddrinfo '%s': %s\n", host_str, gai_strerror(err));
            close(sockfd);
            return 1;
        }

        if (connect(sockfd, res->ai_addr, res->ai_addrlen) < 0) {
            perror("nc: connect");
            freeaddrinfo(res);
            close(sockfd);
            return 1;
        }
        freeaddrinfo(res);
    }

    /* Bidirectional I/O transfer */
    char buf[2048];
    while (1) {
        ssize_t n = recv(active_fd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) break;
        buf[n] = '\0';
        write(STDOUT_FILENO, buf, (size_t)n);
    }

    if (active_fd != sockfd) close(active_fd);
    close(sockfd);
    return 0;
}
