/* ============================================================================
 * AzamiOS Userspace — POSIX Regular Expression Engine (regex.c)
 * File: userland/libc/regex.c
 * ============================================================================ */

#include "include/regex.h"
#include "include/stdlib.h"
#include "include/string.h"
#include "include/ctype.h"
#include "include/stdio.h"

typedef struct {
    char *pattern;
    int   cflags;
} compiled_regex_t;

int regcomp(regex_t *preg, const char *pattern, int cflags)
{
    if (!preg || !pattern) return REG_BADPAT;

    compiled_regex_t *cr = (compiled_regex_t *)malloc(sizeof(compiled_regex_t));
    if (!cr) return REG_ESPACE;

    cr->pattern = strdup(pattern);
    if (!cr->pattern) {
        free(cr);
        return REG_ESPACE;
    }
    cr->cflags = cflags;

    preg->re_compiled = cr;
    preg->re_cflags   = cflags;
    preg->re_nsub     = 0;

    return REG_NOERROR;
}

static int char_matches(char pat_c, char str_c, int icase)
{
    if (pat_c == '.') return (str_c != '\0' && str_c != '\n');
    if (icase) return tolower((unsigned char)pat_c) == tolower((unsigned char)str_c);
    return pat_c == str_c;
}

static int match_pattern(const char *pat, const char *text, int icase, const char **end_ptr);

static int match_star(char c, const char *pat, const char *text, int icase, const char **end_ptr)
{
    do {
        if (match_pattern(pat, text, icase, end_ptr)) return 1;
    } while (*text && char_matches(c, *text++, icase));
    return 0;
}

static int match_plus(char c, const char *pat, const char *text, int icase, const char **end_ptr)
{
    if (!*text || !char_matches(c, *text, icase)) return 0;
    text++;
    return match_star(c, pat, text, icase, end_ptr);
}

static int match_pattern(const char *pat, const char *text, int icase, const char **end_ptr)
{
    if (!*pat) {
        if (end_ptr) *end_ptr = text;
        return 1;
    }
    if (pat[0] == '$' && pat[1] == '\0') {
        if (*text == '\0' || *text == '\n') {
            if (end_ptr) *end_ptr = text;
            return 1;
        }
        return 0;
    }
    if (pat[1] == '*') {
        return match_star(pat[0], pat + 2, text, icase, end_ptr);
    }
    if (pat[1] == '+') {
        return match_plus(pat[0], pat + 2, text, icase, end_ptr);
    }
    if (pat[1] == '?') {
        if (match_pattern(pat + 2, text, icase, end_ptr)) return 1;
        if (*text && char_matches(pat[0], *text, icase)) {
            return match_pattern(pat + 2, text + 1, icase, end_ptr);
        }
        return 0;
    }
    if (*text && char_matches(pat[0], *text, icase)) {
        return match_pattern(pat + 1, text + 1, icase, end_ptr);
    }
    return 0;
}

int regexec(const regex_t *preg, const char *string, size_t nmatch, regmatch_t pmatch[], int eflags)
{
    (void)eflags;
    if (!preg || !preg->re_compiled || !string) return REG_NOMATCH;

    compiled_regex_t *cr = (compiled_regex_t *)preg->re_compiled;
    const char *pat = cr->pattern;
    int icase = (cr->cflags & REG_ICASE);

    if (pat[0] == '^') {
        const char *end = NULL;
        if (match_pattern(pat + 1, string, icase, &end)) {
            if (nmatch > 0 && pmatch) {
                pmatch[0].rm_so = 0;
                pmatch[0].rm_eo = (regoff_t)(end - string);
            }
            return REG_NOERROR;
        }
        return REG_NOMATCH;
    }

    const char *cur = string;
    do {
        const char *end = NULL;
        if (match_pattern(pat, cur, icase, &end)) {
            if (nmatch > 0 && pmatch) {
                pmatch[0].rm_so = (regoff_t)(cur - string);
                pmatch[0].rm_eo = (regoff_t)(end - string);
            }
            return REG_NOERROR;
        }
    } while (*cur++);

    return REG_NOMATCH;
}

size_t regerror(int errcode, const regex_t *preg, char *errbuf, size_t errbuf_size)
{
    (void)preg;
    const char *msg = "Regex error";
    switch (errcode) {
        case REG_NOERROR: msg = "Success"; break;
        case REG_NOMATCH: msg = "No match"; break;
        case REG_BADPAT:  msg = "Invalid regular expression"; break;
        case REG_ESPACE:  msg = "Out of memory"; break;
        default:          msg = "Unknown regex error"; break;
    }
    if (errbuf && errbuf_size > 0) {
        strncpy(errbuf, msg, errbuf_size - 1);
        errbuf[errbuf_size - 1] = '\0';
    }
    return strlen(msg) + 1;
}

void regfree(regex_t *preg)
{
    if (preg && preg->re_compiled) {
        compiled_regex_t *cr = (compiled_regex_t *)preg->re_compiled;
        if (cr->pattern) free(cr->pattern);
        free(cr);
        preg->re_compiled = NULL;
    }
}
