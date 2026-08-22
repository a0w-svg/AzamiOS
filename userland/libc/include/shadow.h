/* ============================================================================
 * AzamiOS Userspace — Shadow Password Header (shadow.h)
 * File: userland/libc/include/shadow.h
 * ============================================================================ */
#pragma once

#include "sys/types.h"

struct spwd {
    char *sp_namp;         /* User login name */
    char *sp_pwdp;         /* Encrypted password */
    long  sp_lstchg;       /* Last password change date */
    long  sp_min;          /* Min days between changes */
    long  sp_max;          /* Max days before change */
    long  sp_warn;         /* Days before pwd expires to warn user */
    long  sp_inact;        /* Days after pwd expires until account inactive */
    long  sp_expire;       /* Days since 1970-01-01 when account expires */
    unsigned long sp_flag; /* Reserved */
};

struct spwd *getspnam(const char *name);
struct spwd *getspent(void);
void setspent(void);
void endspent(void);
