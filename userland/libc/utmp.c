/* ============================================================================
 * AzamiOS Userspace — User Accounting Database Implementation (utmp.c)
 * File: userland/libc/utmp.c
 * ============================================================================ */

#include "include/utmpx.h"
#include "include/utmp.h"
#include "include/stdio.h"
#include "include/stdlib.h"
#include "include/string.h"
#include "include/unistd.h"
#include "include/fcntl.h"
#include "include/sys/stat.h"
#include "include/sys/time.h"

static char g_utmp_file[256] = _PATH_UTMPX;
static int g_utmp_fd = -1;
static struct utmpx g_static_utmpx;
static int g_fallback_mode = 0;
static int g_fallback_idx = 0;

void setutxent(void)
{
    if (g_utmp_fd >= 0) {
        lseek(g_utmp_fd, 0, SEEK_SET);
    } else {
        g_utmp_fd = open(g_utmp_file, O_RDWR | O_CREAT, 0644);
        if (g_utmp_fd < 0) {
            g_utmp_fd = open(g_utmp_file, O_RDONLY);
        }
    }
    g_fallback_idx = 0;
    g_fallback_mode = (g_utmp_fd < 0);
}

void endutxent(void)
{
    if (g_utmp_fd >= 0) {
        close(g_utmp_fd);
        g_utmp_fd = -1;
    }
    g_fallback_idx = 0;
}

int utmpxname(const char *file)
{
    if (!file) return -1;
    endutxent();
    strncpy(g_utmp_file, file, sizeof(g_utmp_file) - 1);
    g_utmp_file[sizeof(g_utmp_file) - 1] = '\0';
    return 0;
}

struct utmpx *getutxent(void)
{
    if (g_utmp_fd < 0 && !g_fallback_mode) {
        setutxent();
    }

    if (g_utmp_fd >= 0) {
        ssize_t n = read(g_utmp_fd, &g_static_utmpx, sizeof(struct utmpx));
        if (n == sizeof(struct utmpx)) {
            return &g_static_utmpx;
        }
    }

    /* Fallback synthesized session entry if utmp file is empty/missing */
    if (g_fallback_idx == 0) {
        g_fallback_idx++;
        memset(&g_static_utmpx, 0, sizeof(struct utmpx));
        g_static_utmpx.ut_type = USER_PROCESS;
        g_static_utmpx.ut_pid = getpid();
        strncpy(g_static_utmpx.ut_line, "tty1", sizeof(g_static_utmpx.ut_line) - 1);
        strncpy(g_static_utmpx.ut_id, "1", sizeof(g_static_utmpx.ut_id) - 1);
        
        char *user = getlogin();
        if (!user || !*user) user = getenv("USER");
        if (!user || !*user) user = "root";
        strncpy(g_static_utmpx.ut_user, user, sizeof(g_static_utmpx.ut_user) - 1);
        strncpy(g_static_utmpx.ut_host, ":0", sizeof(g_static_utmpx.ut_host) - 1);
        gettimeofday(&g_static_utmpx.ut_tv, NULL);
        return &g_static_utmpx;
    }

    return NULL;
}

struct utmpx *getutxid(const struct utmpx *id)
{
    if (!id) return NULL;
    struct utmpx *u;
    while ((u = getutxent()) != NULL) {
        if (id->ut_type == RUN_LVL || id->ut_type == BOOT_TIME ||
            id->ut_type == NEW_TIME || id->ut_type == OLD_TIME) {
            if (u->ut_type == id->ut_type) return u;
        } else if (id->ut_type == INIT_PROCESS || id->ut_type == LOGIN_PROCESS ||
                   id->ut_type == USER_PROCESS || id->ut_type == DEAD_PROCESS) {
            if ((u->ut_type == INIT_PROCESS || u->ut_type == LOGIN_PROCESS ||
                 u->ut_type == USER_PROCESS || u->ut_type == DEAD_PROCESS) &&
                strncmp(u->ut_id, id->ut_id, sizeof(u->ut_id)) == 0) {
                return u;
            }
        }
    }
    return NULL;
}

struct utmpx *getutxline(const struct utmpx *line)
{
    if (!line) return NULL;
    struct utmpx *u;
    while ((u = getutxent()) != NULL) {
        if ((u->ut_type == LOGIN_PROCESS || u->ut_type == USER_PROCESS) &&
            strncmp(u->ut_line, line->ut_line, sizeof(u->ut_line)) == 0) {
            return u;
        }
    }
    return NULL;
}

struct utmpx *pututxline(const struct utmpx *ut)
{
    if (!ut) return NULL;
    if (g_utmp_fd < 0) setutxent();
    if (g_utmp_fd < 0) return NULL;

    /* Search existing slot or append */
    struct utmpx cur;
    off_t offset = 0;
    while (read(g_utmp_fd, &cur, sizeof(struct utmpx)) == sizeof(struct utmpx)) {
        if (cur.ut_type == ut->ut_type && strncmp(cur.ut_id, ut->ut_id, sizeof(cur.ut_id)) == 0) {
            lseek(g_utmp_fd, offset, SEEK_SET);
            write(g_utmp_fd, ut, sizeof(struct utmpx));
            memcpy(&g_static_utmpx, ut, sizeof(struct utmpx));
            return &g_static_utmpx;
        }
        offset += sizeof(struct utmpx);
    }

    lseek(g_utmp_fd, 0, SEEK_END);
    write(g_utmp_fd, ut, sizeof(struct utmpx));
    memcpy(&g_static_utmpx, ut, sizeof(struct utmpx));
    return &g_static_utmpx;
}

/* ── utmp compatibility aliases ───────────────────────────────────────────── */
void setutent(void) { setutxent(); }
void endutent(void) { endutxent(); }
struct utmp *getutent(void) { return getutxent(); }
struct utmp *getutid(const struct utmp *id) { return getutxid(id); }
struct utmp *getutline(const struct utmp *line) { return getutxline(line); }
struct utmp *pututline(const struct utmp *ut) { return pututxline(ut); }
int utmpname(const char *file) { return utmpxname(file); }

void updwtmp(const char *wfile, const struct utmp *ut)
{
    if (!wfile || !ut) return;
    int fd = open(wfile, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        write(fd, ut, sizeof(struct utmp));
        close(fd);
    }
}

void logwtmp(const char *line, const char *name, const char *host)
{
    struct utmp ut;
    memset(&ut, 0, sizeof(ut));
    ut.ut_pid = getpid();
    ut.ut_type = (name && *name) ? USER_PROCESS : DEAD_PROCESS;
    if (line) strncpy(ut.ut_line, line, sizeof(ut.ut_line) - 1);
    if (name) strncpy(ut.ut_user, name, sizeof(ut.ut_user) - 1);
    if (host) strncpy(ut.ut_host, host, sizeof(ut.ut_host) - 1);
    gettimeofday(&ut.ut_tv, NULL);
    updwtmp(_PATH_WTMPX, &ut);
}

int login_tty(int fd)
{
    setsid();
    if (dup2(fd, 0) < 0 || dup2(fd, 1) < 0 || dup2(fd, 2) < 0) return -1;
    if (fd > 2) close(fd);
    return 0;
}
