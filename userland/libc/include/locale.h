/* ============================================================================
 * AzamiOS Userspace — Localization Header (locale.h)
 * File: userland/libc/include/locale.h
 *
 * Provides:
 *   • LC_* category constants + LC_*_MASK POSIX.1-2008 bitmasks
 *   • struct lconv  — numeric and monetary formatting parameters
 *   • setlocale() / localeconv() — category switching and formatting info
 *   • locale_t (opaque), newlocale(), uselocale(), duplocale(), freelocale()
 *     — POSIX.1-2008 extended locale API (lightweight, per-thread stubs)
 * ============================================================================ */
#pragma once

/* ── Category constants ──────────────────────────────────────────────────── */
#define LC_CTYPE          0
#define LC_NUMERIC        1
#define LC_TIME           2
#define LC_COLLATE        3
#define LC_MONETARY       4
#define LC_MESSAGES       5
#define LC_ALL            6

/* ── POSIX.1-2008 bitmask constants ──────────────────────────────────────── */
#define LC_CTYPE_MASK     (1 << LC_CTYPE)
#define LC_NUMERIC_MASK   (1 << LC_NUMERIC)
#define LC_TIME_MASK      (1 << LC_TIME)
#define LC_COLLATE_MASK   (1 << LC_COLLATE)
#define LC_MONETARY_MASK  (1 << LC_MONETARY)
#define LC_MESSAGES_MASK  (1 << LC_MESSAGES)
#define LC_ALL_MASK       (LC_CTYPE_MASK | LC_NUMERIC_MASK | LC_TIME_MASK | \
                           LC_COLLATE_MASK | LC_MONETARY_MASK | LC_MESSAGES_MASK)

/* ── struct lconv ────────────────────────────────────────────────────────── */
struct lconv {
    /* Numeric (non-monetary) formatting */
    char *decimal_point;       /* "."  (C locale) */
    char *thousands_sep;       /* ""   (C locale) */
    char *grouping;            /* ""   (C locale) */

    /* Monetary formatting — international */
    char *int_curr_symbol;     /* "" */
    char *currency_symbol;     /* "" */
    char *mon_decimal_point;   /* "" */
    char *mon_thousands_sep;   /* "" */
    char *mon_grouping;        /* "" */
    char *positive_sign;       /* "" */
    char *negative_sign;       /* "" */

    char  int_frac_digits;     /* CHAR_MAX */
    char  frac_digits;         /* CHAR_MAX */
    char  p_cs_precedes;       /* CHAR_MAX */
    char  p_sep_by_space;      /* CHAR_MAX */
    char  n_cs_precedes;       /* CHAR_MAX */
    char  n_sep_by_space;      /* CHAR_MAX */
    char  p_sign_posn;         /* CHAR_MAX */
    char  n_sign_posn;         /* CHAR_MAX */

    /* POSIX.1-2008 international variant fields */
    char  int_p_cs_precedes;   /* CHAR_MAX */
    char  int_n_cs_precedes;   /* CHAR_MAX */
    char  int_p_sep_by_space;  /* CHAR_MAX */
    char  int_n_sep_by_space;  /* CHAR_MAX */
    char  int_p_sign_posn;     /* CHAR_MAX */
    char  int_n_sign_posn;     /* CHAR_MAX */
};

/* ── Core API ────────────────────────────────────────────────────────────── */
char         *setlocale(int category, const char *locale);
struct lconv *localeconv(void);

/* ── locale_t — POSIX.1-2008 per-object locale ───────────────────────────── */
/* The type is opaque; internally it is a pointer to a locale_entry_t,
 * but callers must treat it as opaque per the standard.                     */
typedef struct _az_locale_obj *locale_t;

#define LC_GLOBAL_LOCALE ((locale_t)-1)

locale_t  newlocale(int category_mask, const char *locale, locale_t base);
locale_t  uselocale(locale_t newloc);
locale_t  duplocale(locale_t locobj);
void      freelocale(locale_t locobj);
