/* ============================================================================
 * AzamiOS Userspace — POSIX Regular Expressions (regex.h)
 * File: userland/libc/include/regex.h
 * ============================================================================ */
#pragma once

#include "sys/types.h"

typedef ssize_t regoff_t;

typedef struct {
    size_t re_nsub;     /* Number of parenthesized subexpressions */
    void  *re_compiled; /* Internal compiled bytecode/pattern */
    int    re_cflags;   /* Compilation flags */
} regex_t;

typedef struct {
    regoff_t rm_so; /* Start offset */
    regoff_t rm_eo; /* End offset */
} regmatch_t;

/* Compilation flags */
#define REG_EXTENDED 1
#define REG_ICASE    2
#define REG_NOSUB    4
#define REG_NEWLINE  8

/* Execution flags */
#define REG_NOTBOL   1
#define REG_NOTEOL   2
#define REG_STARTEND 4

/* Error codes */
enum {
    REG_NOERROR = 0,
    REG_NOMATCH,
    REG_BADPAT,
    REG_ECOLLATE,
    REG_ECTYPE,
    REG_EESCAPE,
    REG_ESUBREG,
    REG_EBRACK,
    REG_EPAREN,
    REG_EBRACE,
    REG_BADBR,
    REG_ERANGE,
    REG_ESPACE,
    REG_BADRPT
};

int    regcomp(regex_t *preg, const char *pattern, int cflags);
int    regexec(const regex_t *preg, const char *string, size_t nmatch, regmatch_t pmatch[], int eflags);
size_t regerror(int errcode, const regex_t *preg, char *errbuf, size_t errbuf_size);
void   regfree(regex_t *preg);
