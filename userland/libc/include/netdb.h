/* ============================================================================
 * AzamiOS Userland Libc — Network Database & Resolver Header (<netdb.h>)
 * File: userland/libc/include/netdb.h
 * ============================================================================ */
#pragma once

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

struct hostent {
    char  *h_name;       /* Official name of host */
    char **h_aliases;    /* Alias list */
    int    h_addrtype;   /* Host address type (AF_INET) */
    int    h_length;     /* Length of address in bytes (4) */
    char **h_addr_list;  /* List of addresses from name server */
};
#define h_addr h_addr_list[0] /* Address, for backward compatibility */

struct addrinfo {
    int              ai_flags;
    int              ai_family;
    int              ai_socktype;
    int              ai_protocol;
    socklen_t        ai_addrlen;
    struct sockaddr *ai_addr;
    char            *ai_canonname;
    struct addrinfo *ai_next;
};

/* Flags for getaddrinfo hints */
#define AI_PASSIVE     0x0001
#define AI_CANONNAME   0x0002
#define AI_NUMERICHOST 0x0004
#define AI_NUMERICSERV 0x0008

/* Error codes for getaddrinfo */
#define EAI_BADFLAGS   -1
#define EAI_NONAME     -2
#define EAI_AGAIN      -3
#define EAI_FAIL       -4
#define EAI_FAMILY     -6
#define EAI_SOCKTYPE   -7
#define EAI_SERVICE    -8
#define EAI_MEMORY     -10
#define EAI_SYSTEM     -11

struct hostent *gethostbyname(const char *name);
int             getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res);
void            freeaddrinfo(struct addrinfo *res);
const char     *gai_strerror(int errcode);
