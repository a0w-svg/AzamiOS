/* ============================================================================
 * AzamiOS Userspace — Filename Pattern Matching (fnmatch.c)
 * File: userland/libc/fnmatch.c
 * ============================================================================ */

#include "include/fnmatch.h"
#include "include/ctype.h"
#include "include/string.h"

int fnmatch(const char *pattern, const char *string, int flags)
{
    if (!pattern || !string) return FNM_NOMATCH;

    const char *p = pattern;
    const char *s = string;

    while (*p) {
        if (*p == '*') {
            while (*p == '*') p++;
            if (!*p) {
                if ((flags & FNM_PATHNAME) && strchr(s, '/')) return FNM_NOMATCH;
                return 0;
            }
            while (*s) {
                if (fnmatch(p, s, flags) == 0) return 0;
                if ((flags & FNM_PATHNAME) && *s == '/') return FNM_NOMATCH;
                s++;
            }
            return FNM_NOMATCH;
        } else if (*p == '?') {
            if (!*s) return FNM_NOMATCH;
            if ((flags & FNM_PATHNAME) && *s == '/') return FNM_NOMATCH;
            if ((flags & FNM_PERIOD) && *s == '.' && (s == string || ((flags & FNM_PATHNAME) && *(s - 1) == '/'))) return FNM_NOMATCH;
            p++;
            s++;
        } else if (*p == '[') {
            p++;
            int negate = (*p == '!' || *p == '^');
            if (negate) p++;
            int match = 0;
            char prev = 0;
            while (*p && *p != ']') {
                if (*p == '-' && prev && p[1] && p[1] != ']') {
                    char next = p[1];
                    char sc = (flags & FNM_CASEFOLD) ? tolower((unsigned char)*s) : *s;
                    char p1 = (flags & FNM_CASEFOLD) ? tolower((unsigned char)prev) : prev;
                    char p2 = (flags & FNM_CASEFOLD) ? tolower((unsigned char)next) : next;
                    if (sc >= p1 && sc <= p2) match = 1;
                    p += 2;
                } else {
                    char sc = (flags & FNM_CASEFOLD) ? tolower((unsigned char)*s) : *s;
                    char pc = (flags & FNM_CASEFOLD) ? tolower((unsigned char)*p) : *p;
                    if (sc == pc) match = 1;
                    prev = *p++;
                }
            }
            if (*p == ']') p++;
            if (negate) match = !match;
            if (!match || !*s) return FNM_NOMATCH;
            s++;
        } else if (*p == '\\' && !(flags & FNM_NOESCAPE)) {
            p++;
            if (*p != *s) return FNM_NOMATCH;
            p++;
            s++;
        } else {
            char sc = (flags & FNM_CASEFOLD) ? tolower((unsigned char)*s) : *s;
            char pc = (flags & FNM_CASEFOLD) ? tolower((unsigned char)*p) : *p;
            if (sc != pc) return FNM_NOMATCH;
            p++;
            s++;
        }
    }

    return (*s == '\0') ? 0 : FNM_NOMATCH;
}
