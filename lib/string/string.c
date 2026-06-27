/**
 * lib/string/string.c  –  AzamiOS portable string & memory implementation
 *
 * Pure C / inline-asm (x86 STOS/MOVS) string primitives.
 * No kernel headers, no port I/O, no ring-0 primitives.
 * Compiles with: i686-elf-gcc -ffreestanding  OR  host gcc for unit testing.
 */
#include "string.h"
#include <stdint.h>
#include <limits.h>

/* ── Memory operations ───────────────────────────────────────────────── */

/**
 * memset – fill the first n bytes of s with the byte value c.
 */
void* memset(void* s, int c, size_t n)
{
    void *orig = s;
    asm volatile("rep stosb" : "+D"(s), "+c"(n) : "a"(c) : "memory");
    return orig;
}

/**
 * memcpy – copy n bytes from src to dest (no overlap assumed).
 */
void* memcpy(void* dest, const void* src, size_t n) {
    void *orig = dest;
    asm volatile("rep movsb" : "+D"(dest), "+S"(src), "+c"(n) : : "memory");
    return orig;
}

/**
 * memcmp – compare two memory regions byte-by-byte.
 * Returns 0 if equal, <0 / >0 otherwise.
 */
int memcmp(const void* s1, const void* s2, size_t n) {
    const unsigned char* p1 = (const unsigned char*)s1;
    const unsigned char* p2 = (const unsigned char*)s2;
    while (n--) {
        if (*p1 != *p2)
            return *p1 - *p2;
        p1++;
        p2++;
    }
    return 0;
}

/**
 * memmove – copy n bytes from src to dest; regions may overlap.
 * Uses fast memcpy when safe, otherwise copies from the end.
 */
void* memmove(void* dest, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dest;
    const uint8_t* s = (const uint8_t*)src;

    if (d <= s || d >= (s + n))
        return memcpy(dest, src, n);

    /* Overlapping: copy backwards */
    d += n;
    s += n;
    while (n--)
        *--d = *--s;
    return dest;
}

/* ── String operations ───────────────────────────────────────────────── */

/**
 * strlen – return the number of bytes in s (not counting the NUL).
 */
size_t strlen(const char* s)
{
    size_t len = 0;
    while (s[len])
        len++;
    return len;
}

/**
 * strcmp – lexicographic comparison of two NUL-terminated strings.
 * Returns 0 if equal, negative if s1 < s2, positive if s1 > s2.
 */
int strcmp(const char* s1, const char* s2)
{
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

/**
 * strcpy – copy src (including NUL) into dest; return dest.
 */
char* strcpy(char* dest, const char* src) {
    char* d = dest;
    while ((*d++ = *src++));
    return dest;
}

/**
 * strncpy – copy at most num characters of src into dest.
 * Pads with NULs if src is shorter than num.
 */
char* strncpy(char* destination, const char* source, size_t num)
{
    char* begin = destination;
    while (num > 0 && *source != '\0') {
        *destination++ = *source++;
        num--;
    }
    while (num > 0) {
        *destination++ = '\0';
        num--;
    }
    return begin;
}

/**
 * strncmp – compare at most n bytes of s1 and s2.
 */
int strncmp(const char* s1, const char* s2, size_t n) {
    while (n && *s1 && (*s1 == *s2)) {
        s1++;
        s2++;
        n--;
    }
    return (n == 0) ? 0 : (*(const unsigned char*)s1 - *(const unsigned char*)s2);
}
