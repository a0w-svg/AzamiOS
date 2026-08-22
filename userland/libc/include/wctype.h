/* ============================================================================
 * AzamiOS Userspace — Wide Character Classification (wctype.h)
 * File: userland/libc/include/wctype.h
 * ============================================================================ */
#pragma once

#include "sys/types.h"

typedef int wint_t;
typedef unsigned long wctype_t;
typedef const int *wctrans_t;

#define WEOF ((wint_t)-1)

int      iswalnum(wint_t wc);
int      iswalpha(wint_t wc);
int      iswblank(wint_t wc);
int      iswcntrl(wint_t wc);
int      iswdigit(wint_t wc);
int      iswgraph(wint_t wc);
int      iswlower(wint_t wc);
int      iswprint(wint_t wc);
int      iswpunct(wint_t wc);
int      iswspace(wint_t wc);
int      iswupper(wint_t wc);
int      iswxdigit(wint_t wc);
wint_t   towlower(wint_t wc);
wint_t   towupper(wint_t wc);
wctype_t wctype(const char *name);
int      iswctype(wint_t wc, wctype_t desc);
