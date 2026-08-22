/* ============================================================================
 * AzamiOS Userspace — Group Database Header (grp.h)
 * File: userland/libc/include/grp.h
 * ============================================================================ */
#pragma once

#include "sys/types.h"

struct group {
    char   *gr_name;    /* Group name */
    char   *gr_passwd;  /* Password hash or x */
    gid_t   gr_gid;     /* Group ID */
    char  **gr_mem;     /* Array of member usernames */
};

struct group *getgrnam(const char *name);
struct group *getgrgid(gid_t gid);
struct group *getgrent(void);
void          setgrent(void);
void          endgrent(void);
