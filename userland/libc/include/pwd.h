/* ============================================================================
 * AzamiOS Userspace — User Database Header (pwd.h)
 * File: userland/libc/include/pwd.h
 * ============================================================================ */
#pragma once

#include "sys/types.h"

struct passwd {
    char   *pw_name;    /* Username */
    char   *pw_passwd;  /* Password hash or x */
    uid_t   pw_uid;     /* User ID */
    gid_t   pw_gid;     /* Group ID */
    char   *pw_gecos;   /* Real name / info */
    char   *pw_dir;     /* Home directory */
    char   *pw_shell;   /* Login shell */
};

struct passwd *getpwnam(const char *name);
struct passwd *getpwuid(uid_t uid);
struct passwd *getpwent(void);
void           setpwent(void);
void           endpwent(void);
