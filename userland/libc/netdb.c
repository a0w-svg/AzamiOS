/* ============================================================================
 * AzamiOS Userland Libc — DNS Resolver & Network Database (netdb.c)
 * File: userland/libc/netdb.c
 *
 * Implements RFC 1035 UDP DNS client resolver, gethostbyname, getaddrinfo,
 * freeaddrinfo, and gai_strerror.
 * ============================================================================ */

#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>

#include <fcntl.h>
#include <sys/ioctl.h>

#define DEFAULT_DNS_IP "8.8.8.8"
#define DNS_PORT       53

static void dns_get_nameserver(char *out_ip, size_t max_len)
{
    /* 1. Try reading /etc/resolv.conf */
    int fd = open("/etc/resolv.conf", O_RDONLY, 0);
    if (fd >= 0) {
        char buf[512];
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n > 0) {
            buf[n] = '\0';
            char *line = strtok(buf, "\r\n");
            while (line) {
                while (*line == ' ' || *line == '\t') line++;
                if (strncmp(line, "nameserver", 10) == 0) {
                    char *ip_str = line + 10;
                    while (*ip_str == ' ' || *ip_str == '\t') ip_str++;
                    if (*ip_str) {
                        strncpy(out_ip, ip_str, max_len - 1);
                        out_ip[max_len - 1] = '\0';
                        return;
                    }
                }
                line = strtok(NULL, "\r\n");
            }
        }
    }

    /* 2. Try querying kernel via /dev/net0 IOCTL */
    int net_fd = open("/dev/net0", O_RDWR, 0);
    if (net_fd >= 0) {
        unsigned char dns_bytes[4] = { 0, 0, 0, 0 };
        if (ioctl(net_fd, 0x891f /* SIOCGIFDNS */, (unsigned long)dns_bytes) == 0) {
            if (dns_bytes[0] != 0 || dns_bytes[1] != 0 || dns_bytes[2] != 0 || dns_bytes[3] != 0) {
                snprintf(out_ip, max_len, "%u.%u.%u.%u",
                         dns_bytes[0], dns_bytes[1], dns_bytes[2], dns_bytes[3]);
                close(net_fd);
                return;
            }
        }
        close(net_fd);
    }

    /* 3. Fallback */
    strncpy(out_ip, DEFAULT_DNS_IP, max_len - 1);
    out_ip[max_len - 1] = '\0';
}

typedef struct __attribute__((packed)) {
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t arcount;
} dns_hdr_t;

/* Format domain name into DNS wire format: "www.google.com" -> "\x03www\x06google\x03com\0" */
static int dns_format_qname(const char *domain, unsigned char *out, size_t out_max)
{
    size_t out_idx = 0;
    const char *src = domain;

    while (*src) {
        const char *dot = strchr(src, '.');
        size_t len = dot ? (size_t)(dot - src) : strlen(src);
        if (len == 0 || len > 63 || (out_idx + 1 + len + 1) >= out_max) {
            return -1;
        }

        out[out_idx++] = (unsigned char)len;
        memcpy(out + out_idx, src, len);
        out_idx += len;

        if (!dot) break;
        src = dot + 1;
    }

    out[out_idx++] = 0;
    return (int)out_idx;
}

static int dns_resolve_ipv4(const char *hostname, struct in_addr *out_addr)
{
    if (!hostname || !out_addr) return -1;

    /* 1. Direct numeric IPv4 check */
    if (inet_pton(AF_INET, hostname, out_addr) == 1) {
        return 0;
    }

    /* 2. Localhost lookup */
    if (strcmp(hostname, "localhost") == 0) {
        out_addr->s_addr = htonl(0x7f000001); /* 127.0.0.1 */
        return 0;
    }

    /* 3. Dynamic DNS server lookup from system configuration */
    char dns_ip_str[64];
    dns_get_nameserver(dns_ip_str, sizeof(dns_ip_str));

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return -1;

    struct sockaddr_in dns_serv;
    memset(&dns_serv, 0, sizeof(dns_serv));
    dns_serv.sin_family = AF_INET;
    dns_serv.sin_port = htons(DNS_PORT);
    dns_serv.sin_addr.s_addr = inet_addr(dns_ip_str);

    unsigned char query_buf[512];
    dns_hdr_t *hdr = (dns_hdr_t *)query_buf;
    hdr->id = htons(0x4242);
    hdr->flags = htons(0x0100); /* Standard query with recursion desired */
    hdr->qdcount = htons(1);
    hdr->ancount = 0;
    hdr->nscount = 0;
    hdr->arcount = 0;

    int qname_len = dns_format_qname(hostname, query_buf + sizeof(dns_hdr_t), sizeof(query_buf) - sizeof(dns_hdr_t));
    if (qname_len < 0) {
        close(sock);
        return -1;
    }

    size_t offset = sizeof(dns_hdr_t) + qname_len;
    uint16_t qtype = htons(1);  /* Type A (IPv4) */
    uint16_t qclass = htons(1); /* Class IN */
    memcpy(query_buf + offset, &qtype, 2);
    memcpy(query_buf + offset + 2, &qclass, 2);
    offset += 4;

    /* Send query */
    ssize_t sent = sendto(sock, query_buf, offset, 0, (struct sockaddr *)&dns_serv, sizeof(dns_serv));
    if (sent <= 0) {
        close(sock);
        return -1;
    }

    /* Receive answer */
    unsigned char resp_buf[512];
    ssize_t resp_len = recvfrom(sock, resp_buf, sizeof(resp_buf), 0, NULL, NULL);
    close(sock);

    if (resp_len < (ssize_t)sizeof(dns_hdr_t)) {
        return -1;
    }

    const dns_hdr_t *resp_hdr = (const dns_hdr_t *)resp_buf;
    uint16_t ancount = ntohs(resp_hdr->ancount);
    if (ancount == 0) return -1;

    /* Skip question section in response */
    const unsigned char *ptr = resp_buf + sizeof(dns_hdr_t);
    const unsigned char *end = resp_buf + resp_len;

    while (ptr < end && *ptr != 0) {
        if ((*ptr & 0xC0) == 0xC0) { ptr += 2; break; } /* Compressed pointer */
        ptr += (*ptr + 1);
    }
    if (ptr < end && *ptr == 0) ptr++;
    ptr += 4; /* Skip QTYPE and QCLASS */

    /* Parse Answer Records */
    for (int i = 0; i < ancount && ptr < end; i++) {
        /* Skip NAME */
        if ((*ptr & 0xC0) == 0xC0) {
            ptr += 2;
        } else {
            while (ptr < end && *ptr != 0) ptr += (*ptr + 1);
            if (ptr < end) ptr++;
        }

        if (ptr + 10 > end) break;
        uint16_t atype = ntohs(*(const uint16_t *)ptr);
        uint16_t rdlen = ntohs(*(const uint16_t *)(ptr + 8));
        ptr += 10;

        if (atype == 1 && rdlen == 4 && ptr + 4 <= end) {
            memcpy(&out_addr->s_addr, ptr, 4);
            return 0;
        }
        ptr += rdlen;
    }

    return -1;
}

static struct hostent g_hostent;
static char *g_host_aliases[1] = { NULL };
static char *g_host_addrs[2] = { NULL, NULL };
static struct in_addr g_host_addr;
static char g_host_name[256];

struct hostent *gethostbyname(const char *name)
{
    if (!name) return NULL;

    struct in_addr addr;
    if (dns_resolve_ipv4(name, &addr) != 0) {
        return NULL;
    }

    strncpy(g_host_name, name, sizeof(g_host_name) - 1);
    g_host_name[sizeof(g_host_name) - 1] = '\0';
    g_host_addr = addr;

    g_host_addrs[0] = (char *)&g_host_addr;
    g_host_addrs[1] = NULL;

    g_hostent.h_name = g_host_name;
    g_hostent.h_aliases = g_host_aliases;
    g_hostent.h_addrtype = AF_INET;
    g_hostent.h_length = sizeof(struct in_addr);
    g_hostent.h_addr_list = g_host_addrs;

    return &g_hostent;
}

int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res)
{
    if (!res || (!node && !service)) return EAI_NONAME;

    struct in_addr addr;
    memset(&addr, 0, sizeof(addr));

    if (node) {
        if (dns_resolve_ipv4(node, &addr) != 0) {
            return EAI_NONAME;
        }
    } else {
        addr.s_addr = htonl(INADDR_ANY);
    }

    unsigned short port = 0;
    if (service) {
        if (strcmp(service, "http") == 0) port = 80;
        else if (strcmp(service, "https") == 0) port = 443;
        else if (strcmp(service, "dns") == 0 || strcmp(service, "domain") == 0) port = 53;
        else if (strcmp(service, "ftp") == 0) port = 21;
        else if (strcmp(service, "ssh") == 0) port = 22;
        else if (strcmp(service, "telnet") == 0) port = 23;
        else port = (unsigned short)atoi(service);
    }

    struct addrinfo *ai = (struct addrinfo *)calloc(1, sizeof(struct addrinfo));
    if (!ai) return EAI_MEMORY;

    struct sockaddr_in *sin = (struct sockaddr_in *)calloc(1, sizeof(struct sockaddr_in));
    if (!sin) {
        free(ai);
        return EAI_MEMORY;
    }

    sin->sin_family = AF_INET;
    sin->sin_port = htons(port);
    sin->sin_addr = addr;

    ai->ai_family = AF_INET;
    ai->ai_socktype = hints ? hints->ai_socktype : SOCK_STREAM;
    ai->ai_protocol = hints ? hints->ai_protocol : (ai->ai_socktype == SOCK_DGRAM ? IPPROTO_UDP : IPPROTO_TCP);
    ai->ai_addrlen = sizeof(struct sockaddr_in);
    ai->ai_addr = (struct sockaddr *)sin;
    ai->ai_canonname = node ? strdup(node) : NULL;
    ai->ai_next = NULL;

    *res = ai;
    return 0;
}

void freeaddrinfo(struct addrinfo *res)
{
    while (res) {
        struct addrinfo *next = res->ai_next;
        if (res->ai_addr) free(res->ai_addr);
        if (res->ai_canonname) free(res->ai_canonname);
        free(res);
        res = next;
    }
}

const char *gai_strerror(int errcode)
{
    switch (errcode) {
    case 0: return "Success";
    case EAI_NONAME: return "Name or service not known";
    case EAI_AGAIN: return "Temporary failure in name resolution";
    case EAI_FAIL: return "Non-recoverable failure in name resolution";
    case EAI_FAMILY: return "ai_family not supported";
    case EAI_SOCKTYPE: return "ai_socktype not supported";
    case EAI_SERVICE: return "Service not supported for socket type";
    case EAI_MEMORY: return "Memory allocation failure";
    case EAI_SYSTEM: return "System error";
    default: return "Unknown error";
    }
}
