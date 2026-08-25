/* ============================================================================
 * AzamiOS Userspace — User Accounting Database (utmpx.h)
 * File: userland/libc/include/utmpx.h
 * ============================================================================ */
#pragma once

#include "sys/types.h"
#include "sys/time.h"

#define UT_LINESIZE 32
#define UT_NAMESIZE 32
#define UT_HOSTSIZE 256

/* Type of login */
#define EMPTY         0 /* No valid user accounting information */
#define RUN_LVL       1 /* Change in system run-level */
#define BOOT_TIME     2 /* Time of system boot */
#define NEW_TIME      3 /* Time after system clock change */
#define OLD_TIME      4 /* Time before system clock change */
#define INIT_PROCESS  5 /* Process spawned by init */
#define LOGIN_PROCESS 6 /* Session leader process for user login */
#define USER_PROCESS  7 /* Normal user process */
#define DEAD_PROCESS  8 /* Terminated process */
#define ACCOUNTING    9 /* System accounting */

struct exit_status {
    short e_termination; /* Process termination status */
    short e_exit;        /* Process exit status */
};

struct utmpx {
    short              ut_type;              /* Type of entry */
    pid_t              ut_pid;               /* Process ID of login process */
    char               ut_line[UT_LINESIZE]; /* Device name of tty */
    char               ut_id[4];             /* Terminal name suffix or inittab ID */
    char               ut_user[UT_NAMESIZE]; /* Username */
    char               ut_host[UT_HOSTSIZE]; /* Hostname for remote login */
    struct exit_status ut_exit;              /* Exit status of a dead process */
    long               ut_session;           /* Session ID */
    struct timeval     ut_tv;                /* Time entry was made */
    int32_t            ut_addr_v6[4];        /* Internet address of remote host */
    char               __unused[20];         /* Reserved for future use */
};

#define _PATH_UTMPX "/var/run/utmp"
#define _PATH_WTMPX "/var/log/wtmp"

void          setutxent(void);
void          endutxent(void);
struct utmpx *getutxent(void);
struct utmpx *getutxid(const struct utmpx *id);
struct utmpx *getutxline(const struct utmpx *line);
struct utmpx *pututxline(const struct utmpx *ut);
int           utmpxname(const char *file);
