/**
 * lib/string/string.h  –  AzamiOS portable string & memory utilities
 *
 * This header is kernel-independent: it can be compiled with any C compiler
 * targeting i686-elf or the host toolchain for unit testing.
 * It is the canonical definition; kernel/klibc/include/string.h delegates here.
 */
#ifndef LIB_STRING_H
#define LIB_STRING_H

#include <stddef.h>

/* ── Memory operations ───────────────────────────────────────────────── */
void* memset(void* s, int c, size_t n);
void* memcpy(void* dest, const void* src, size_t n);
void* memmove(void* destination, const void* source, size_t num);
int   memcmp(const void* s1, const void* s2, size_t n);

/* ── String operations ───────────────────────────────────────────────── */
size_t strlen(const char* s);
int   strcmp(const char* s1, const char* s2);
int   strncmp(const char* s1, const char* s2, size_t n);
char* strcpy(char* dest, const char* src);
char* strncpy(char* destination, const char* source, size_t num);

#endif /* LIB_STRING_H */
