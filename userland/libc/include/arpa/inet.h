#pragma once

#include <netinet/in.h>
#include <sys/types.h>
#include <stdint.h>

in_addr_t       inet_addr(const char *cp);
int             inet_aton(const char *cp, struct in_addr *inp);
char           *inet_ntoa(struct in_addr in);
int             inet_pton(int af, const char *src, void *dst);
const char     *inet_ntop(int af, const void *src, char *dst, socklen_t size);
in_addr_t       inet_network(const char *cp);
struct in_addr  inet_makeaddr(in_addr_t net, in_addr_t host);
in_addr_t       inet_lnaof(struct in_addr in);
in_addr_t       inet_netof(struct in_addr in);


