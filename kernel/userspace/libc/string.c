#include "include/string.h"
#include <limits.h>
#include <stdint.h>

/* Fill block of memory */
void* memset(void* s, int c, size_t n) {
    uint8_t* p = (uint8_t*)s;
    while (n--) {
        *p++ = (uint8_t)c;
    }
    return s;
}

/* Copy block of memory */
void* memcpy(void* dest, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dest;
    const uint8_t* s = (const uint8_t*)src;
    while (n--) {
        *d++ = *s++;
    }
    return dest;
}

int memcmp(const void* s1, const void* s2, size_t n) {
    const unsigned char* p1 = (const unsigned char*)s1;
    const unsigned char* p2 = (const unsigned char*)s2;
    while (n--) {
        if (*p1 != *p2) {
            return *p1 - *p2;
        }
        p1++;
        p2++;
    }
    return 0;
}


/* Move block of memory (handles overlapping regions) */
void* memmove(void* dest, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dest;
    const uint8_t* s = (const uint8_t*)src;

    if (d <= s || d >= (s + n)) {
        return memcpy(dest, src, n);
    }
    d += n;
    s += n;
    while (n--) {
        *--d = *--s;
    }
    return dest;
}

/* Return the string length */
int strlen(const char* s) {
    size_t len = 0;
    while (s[len]) {
        len++;
    }
    return (int)len;
}

/* Compare two strings */
int strcmp(const char* s1, const char* s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

/* Copy string */
char* strcpy(char* dest, const char* src) {
    char* d = dest;
    while ((*d++ = *src++));
    return dest;
}

/* Copy n characters from string */
char* strncpy(char* destination, const char* source, size_t num) {
    char* begin = destination;
    while (num > 0 && *source) {
        *destination++ = *source++;
        num--;
    }
    while (num > 0) {
        *destination++ = '\0';
        num--;
    }
    return begin;
}

/* Compare n characters of two strings */
int strncmp(const char* s1, const char* s2, size_t n) {
    while (n && *s1 && (*s1 == *s2)) {
        s1++;
        s2++;
        n--;
    }
    return (n == 0) ? 0 : (*(const unsigned char*)s1 - *(const unsigned char*)s2);
}