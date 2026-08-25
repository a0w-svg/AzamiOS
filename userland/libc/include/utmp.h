/* ============================================================================
 * AzamiOS Userspace — User Accounting Database (utmp.h)
 * File: userland/libc/include/utmp.h
 * ============================================================================ */
#pragma once

#include "utmpx.h"

#define ut_name ut_user
#define ut_time ut_tv.tv_sec

#define _PATH_UTMP _PATH_UTMPX
#define _PATH_WTMP _PATH_WTMPX

#define utmp utmpx

void         setutent(void);
void         endutent(void);
struct utmp *getutent(void);
struct utmp *getutid(const struct utmp *id);
struct utmp *getutline(const struct utmp *line);
struct utmp *pututline(const struct utmp *ut);
int          utmpname(const char *file);
void         updwtmp(const char *wfile, const struct utmp *ut);
void         logwtmp(const char *line, const char *name, const char *host);
int          login_tty(int fd);
