/**
 * string.h — AzamiOS libc: complete string & memory interface
 */
#ifndef _STRING_H
#define _STRING_H

#include <stddef.h>
#ifndef NULL
#define NULL ((void*)0)
#endif

/* ── Memory operations ─────────────────────────────────────────── */
void  *memset (void *s, int c, size_t n);
void  *memcpy (void *dest, const void *src, size_t n);
void  *memmove(void *dest, const void *src, size_t n);
int    memcmp (const void *s1, const void *s2, size_t n);
void  *memchr (const void *s, int c, size_t n);

/* ── String measurement & comparison ───────────────────────────── */
size_t strlen (const char *s);
int    strcmp (const char *s1, const char *s2);
int    strncmp(const char *s1, const char *s2, size_t n);
int    strcasecmp (const char *s1, const char *s2);
int    strncasecmp(const char *s1, const char *s2, size_t n);

/* ── String copy & concatenation ───────────────────────────────── */
char  *strcpy (char *dest, const char *src);
char  *strncpy(char *dest, const char *src, size_t n);
char  *strcat (char *dest, const char *src);
char  *strncat(char *dest, const char *src, size_t n);
char  *strdup (const char *s);           /* malloc-backed */

/* ── Search ─────────────────────────────────────────────────────── */
char  *strchr (const char *s, int c);
char  *strrchr(const char *s, int c);
char  *strstr (const char *haystack, const char *needle);
char  *strpbrk(const char *s, const char *accept);
size_t strspn (const char *s, const char *accept);
size_t strcspn(const char *s, const char *reject);

/* ── Tokenising ─────────────────────────────────────────────────── */
char  *strtok  (char *str, const char *delim);
char  *strtok_r(char *str, const char *delim, char **saveptr);

/* ── Conversion ─────────────────────────────────────────────────── */
long        strtol (const char *s, char **end, int base);
unsigned long strtoul(const char *s, char **end, int base);

/* ── Error strings ──────────────────────────────────────────────── */
char  *strerror(int errnum);

#endif /* _STRING_H */