/* ============================================================================
 * AzamiOS Userspace — Word Expansion Implementation (wordexp.c)
 * File: userland/libc/wordexp.c
 * ============================================================================ */

#include "include/wordexp.h"
#include "include/stdlib.h"
#include "include/string.h"
#include "include/ctype.h"

int wordexp(const char *words, wordexp_t *pwordexp, int flags)
{
    if (!words || !pwordexp) return WRDE_SYNTAX;

    size_t count = 0;
    size_t cap = 16;
    char **wordv = (char **)malloc(cap * sizeof(char *));
    if (!wordv) return WRDE_NOSPACE;

    size_t offs = (flags & WRDE_DOOFFS) ? pwordexp->we_offs : 0;
    pwordexp->we_offs = offs;
    for (size_t i = 0; i < offs; i++) {
        wordv[count++] = NULL;
    }

    if (flags & WRDE_APPEND) {
        /* Append mode: keep existing words */
        for (size_t i = offs; i < pwordexp->we_wordc + offs; i++) {
            if (count >= cap) {
                cap *= 2;
                char **nv = (char **)realloc(wordv, cap * sizeof(char *));
                if (!nv) {
                    free(wordv);
                    return WRDE_NOSPACE;
                }
                wordv = nv;
            }
            wordv[count++] = pwordexp->we_wordv[i];
        }
    }

    size_t init_count = count;
    const char *p = words;
    char buf[4096];

    while (*p) {
        /* Skip leading whitespace */
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;

        size_t bpos = 0;
        char quote = 0;

        while (*p && (quote || !isspace((unsigned char)*p))) {
            if (*p == '\\' && !quote) {
                p++;
                if (*p && bpos < sizeof(buf) - 1) buf[bpos++] = *p++;
            } else if (*p == '\'' || *p == '\"') {
                if (!quote) {
                    quote = *p++;
                } else if (quote == *p) {
                    quote = 0;
                    p++;
                } else {
                    if (bpos < sizeof(buf) - 1) buf[bpos++] = *p++;
                }
            } else if (*p == '$' && quote != '\'') {
                p++;
                char varname[256];
                size_t vpos = 0;
                if (*p == '{') {
                    p++;
                    while (*p && *p != '}' && vpos < sizeof(varname) - 1) {
                        varname[vpos++] = *p++;
                    }
                    if (*p == '}') p++;
                } else {
                    while (*p && (isalnum((unsigned char)*p) || *p == '_') && vpos < sizeof(varname) - 1) {
                        varname[vpos++] = *p++;
                    }
                }
                varname[vpos] = '\0';
                const char *val = getenv(varname);
                if (val) {
                    size_t vlen = strlen(val);
                    for (size_t k = 0; k < vlen && bpos < sizeof(buf) - 1; k++) {
                        buf[bpos++] = val[k];
                    }
                } else if (flags & WRDE_UNDEF) {
                    for (size_t i = init_count; i < count; i++) free(wordv[i]);
                    free(wordv);
                    return WRDE_BADVAL;
                }
            } else {
                if (bpos < sizeof(buf) - 1) buf[bpos++] = *p++;
            }
        }

        if (quote) {
            /* Unmatched quote */
            for (size_t i = init_count; i < count; i++) free(wordv[i]);
            free(wordv);
            return WRDE_SYNTAX;
        }

        buf[bpos] = '\0';
        char *w = strdup(buf);
        if (!w) {
            for (size_t i = init_count; i < count; i++) free(wordv[i]);
            free(wordv);
            return WRDE_NOSPACE;
        }

        if (count + 1 >= cap) {
            cap *= 2;
            char **nv = (char **)realloc(wordv, cap * sizeof(char *));
            if (!nv) {
                free(w);
                for (size_t i = init_count; i < count; i++) free(wordv[i]);
                free(wordv);
                return WRDE_NOSPACE;
            }
            wordv = nv;
        }
        wordv[count++] = w;
    }

    wordv[count] = NULL;
    pwordexp->we_wordc = count - offs;
    pwordexp->we_wordv = wordv;
    pwordexp->we_offs  = offs;

    return 0;
}

void wordfree(wordexp_t *pwordexp)
{
    if (!pwordexp || !pwordexp->we_wordv) return;
    size_t offs = pwordexp->we_offs;
    for (size_t i = offs; i < pwordexp->we_wordc + offs; i++) {
        if (pwordexp->we_wordv[i]) {
            free(pwordexp->we_wordv[i]);
        }
    }
    free(pwordexp->we_wordv);
    pwordexp->we_wordv = NULL;
    pwordexp->we_wordc = 0;
    pwordexp->we_offs  = 0;
}
