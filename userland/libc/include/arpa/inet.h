/* ============================================================================
 * AzamiOS Userland Libc — Internet Definitions (<arpa/inet.h>)
 * File: userland/libc/include/arpa/inet.h
 * ============================================================================ */
#pragma once

#include <netinet/in.h>
#include <sys/types.h>

static inline unsigned short htons(unsigned short v) { return (unsigned short)((v << 8) | (v >> 8)); }
static inline unsigned short ntohs(unsigned short v) { return htons(v); }
static inline unsigned int   htonl(unsigned int v)   { return (((v & 0xFF) << 24) | ((v & 0xFF00) << 8) | ((v & 0xFF0000) >> 8) | ((v >> 24) & 0xFF)); }
static inline unsigned int   ntohl(unsigned int v)   { return htonl(v); }

in_addr_t   inet_addr(const char *cp);
char       *inet_ntoa(struct in_addr in);
int         inet_pton(int af, const char *src, void *dst);
const char *inet_ntop(int af, const void *src, char *dst, socklen_t size);
