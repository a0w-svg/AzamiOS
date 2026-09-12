/* ============================================================================
 * AzamiOS — Freestanding String & Memory Utility Header
 * File: kernel/lib/string.h
 * ============================================================================ */
#pragma once

#include "../../include/azami/types.h"

void *memset(void *dest, int c, size_t n);
void *memcpy(void *dest, const void *src, size_t n);
void *memmove(void *dest, const void *src, size_t n);
int   memcmp(const void *ptr1, const void *ptr2, size_t n);
void *memchr(const void *s, int c, size_t n);
size_t strlen(const char *str);
size_t strnlen(const char *str, size_t maxlen);
char  *strcpy(char *dest, const char *src);
char  *strncpy(char *dest, const char *src, size_t n);
char  *strcat(char *dest, const char *src);
char  *strncat(char *dest, const char *src, size_t n);
int    strcmp(const char *s1, const char *s2);
int    strncmp(const char *s1, const char *s2, size_t n);
int    strcasecmp(const char *s1, const char *s2);
int    strncasecmp(const char *s1, const char *s2, size_t n);
char  *strchr(const char *s, int c);
char  *strrchr(const char *s, int c);
char  *strstr(const char *haystack, const char *needle);
size_t strspn(const char *s, const char *accept);
size_t strcspn(const char *s, const char *reject);
char  *strpbrk(const char *s, const char *accept);
char  *strsep(char **stringp, const char *delim);
char  *strtok_r(char *str, const char *delim, char **saveptr);
char  *strdup(const char *s);
char  *strndup(const char *s, size_t n);
int    snprintf(char *buf, size_t size, const char *fmt, ...);
int    vsnprintf(char *buf, size_t size, const char *fmt, __builtin_va_list ap);

/*
 * scnprintf()/vscnprintf() — like snprintf(), but return the number of
 * characters *actually written* (excluding the NUL) rather than the number
 * that would have been written had the buffer been large enough.
 *
 * snprintf()'s C99 return value makes the common accumulator idiom
 *
 *     n += snprintf(buf + n, len - n, ...);
 *
 * silently unsafe: once one call truncates, `n` runs past `len` and every
 * subsequent length computation is wrong — and a caller that treats the final
 * `n` as "bytes in buf" reads off the end of the buffer.  Use scnprintf() for
 * any such loop; `n` then never exceeds `len - 1`.
 */
int    scnprintf(char *buf, size_t size, const char *fmt, ...);
int    vscnprintf(char *buf, size_t size, const char *fmt, __builtin_va_list ap);

/* Security and crypto memory utilities */
void   memzero_explicit(void *s, size_t count);
int    crypto_memneq(const void *a, const void *b, size_t size);
