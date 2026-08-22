/* ============================================================================
 * AzamiOS Userspace — HTTP Client Transfer Utility (curl.elf)
 * File: userland/apps/curl/main.c
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
    fprintf(stderr, "Usage: curl [-o outfile] [-v] [-I] <url>\n");
    fprintf(stderr, "  -o outfile  Write payload to outfile instead of stdout\n");
    fprintf(stderr, "  -v          Verbose header output\n");
    fprintf(stderr, "  -I          Show headers only (HEAD request)\n");
    exit(1);
}

int main(int argc, char **argv)
{
    const char *url_str = NULL;
    const char *outfile = NULL;
    int verbose = 0;
    int headers_only = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            outfile = argv[++i];
        } else if (strcmp(argv[i], "-v") == 0) {
            verbose = 1;
        } else if (strcmp(argv[i], "-I") == 0) {
            headers_only = 1;
        } else if (!url_str) {
            url_str = argv[i];
        }
    }

    if (!url_str) usage();

    /* Parse URL: [http://]host[:port][/path] */
    const char *p = url_str;
    if (strncmp(p, "http://", 7) == 0) {
        p += 7;
    }

    char host[128];
    char path[256];
    int port = 80;

    const char *slash = strchr(p, '/');
    const char *colon = strchr(p, ':');

    if (slash && colon && colon < slash) {
        size_t host_len = (size_t)(colon - p);
        if (host_len >= sizeof(host)) host_len = sizeof(host) - 1;
        strncpy(host, p, host_len);
        host[host_len] = '\0';
        port = atoi(colon + 1);
        strncpy(path, slash, sizeof(path) - 1);
    } else if (colon && !slash) {
        size_t host_len = (size_t)(colon - p);
        if (host_len >= sizeof(host)) host_len = sizeof(host) - 1;
        strncpy(host, p, host_len);
        host[host_len] = '\0';
        port = atoi(colon + 1);
        strcpy(path, "/");
    } else if (slash) {
        size_t host_len = (size_t)(slash - p);
        if (host_len >= sizeof(host)) host_len = sizeof(host) - 1;
        strncpy(host, p, host_len);
        host[host_len] = '\0';
        strncpy(path, slash, sizeof(path) - 1);
    } else {
        strncpy(host, p, sizeof(host) - 1);
        host[sizeof(host) - 1] = '\0';
        strcpy(path, "/");
    }

    if (verbose) {
        fprintf(stderr, "* Connecting to %s port %d...\n", host, port);
    }

    /* Resolve host */
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    int err = getaddrinfo(host, port_str, &hints, &res);
    if (err != 0 || !res) {
        fprintf(stderr, "curl: could not resolve host '%s': %s\n", host, gai_strerror(err));
        return 1;
    }

    int sockfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sockfd < 0) {
        perror("curl: socket");
        freeaddrinfo(res);
        return 1;
    }

    if (connect(sockfd, res->ai_addr, res->ai_addrlen) < 0) {
        perror("curl: connect");
        close(sockfd);
        freeaddrinfo(res);
        return 1;
    }
    freeaddrinfo(res);

    if (verbose) {
        fprintf(stderr, "* Connected! Sending HTTP request...\n");
    }

    /* Construct HTTP Request */
    char req[512];
    int req_len = snprintf(req, sizeof(req),
        "%s %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: AzamiOS-Curl/1.0\r\n"
        "Accept: */*\r\n"
        "Connection: close\r\n\r\n",
        headers_only ? "HEAD" : "GET",
        path,
        host
    );

    if (send(sockfd, req, (size_t)req_len, 0) < 0) {
        perror("curl: send");
        close(sockfd);
        return 1;
    }

    int out_fd = STDOUT_FILENO;
    if (outfile) {
        out_fd = open(outfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (out_fd < 0) {
            perror("curl: open outfile");
            close(sockfd);
            return 1;
        }
    }

    /* Receive HTTP Response */
    char buf[4096];
    int header_done = 0;
    ssize_t n;

    while ((n = recv(sockfd, buf, sizeof(buf) - 1, 0)) > 0) {
        buf[n] = '\0';

        if (!header_done) {
            char *header_end = strstr(buf, "\r\n\r\n");
            if (header_end) {
                header_done = 1;
                size_t hlen = (size_t)(header_end - buf) + 4;
                if (verbose || headers_only) {
                    write(STDERR_FILENO, buf, hlen);
                }
                if (!headers_only && (size_t)n > hlen) {
                    write(out_fd, buf + hlen, (size_t)n - hlen);
                }
            } else {
                if (verbose || headers_only) {
                    write(STDERR_FILENO, buf, (size_t)n);
                }
            }
        } else {
            if (!headers_only) {
                write(out_fd, buf, (size_t)n);
            }
        }
    }

    if (out_fd != STDOUT_FILENO) close(out_fd);
    close(sockfd);

    if (verbose) {
        fprintf(stderr, "* Connection closed. Transfer complete.\n");
    }

    return 0;
}
