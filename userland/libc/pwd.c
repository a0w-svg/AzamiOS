/* ============================================================================
 * AzamiOS Userspace — User Database Implementation (pwd.c)
 * File: userland/libc/pwd.c
 * ============================================================================ */

#include "include/pwd.h"
#include "include/stdio.h"
#include "include/stdlib.h"
#include "include/string.h"
#include "include/unistd.h"
#include "include/fcntl.h"

static FILE *s_pw_file = NULL;
static struct passwd s_pw_buf;
static char s_line_buf[256];

void setpwent(void)
{
    if (s_pw_file) {
        rewind(s_pw_file);
    } else {
        s_pw_file = fopen("/etc/passwd", "r");
    }
}

void endpwent(void)
{
    if (s_pw_file) {
        fclose(s_pw_file);
        s_pw_file = NULL;
    }
}

static struct passwd *parse_passwd_line(char *line)
{
    if (!line) return NULL;
    /* Format: username:password:uid:gid:gecos:dir:shell */
    char *fields[7];
    char *cur = line;
    for (int i = 0; i < 7; i++) {
        fields[i] = cur;
        char *colon = strchr(cur, ':');
        if (colon) {
            *colon = '\0';
            cur = colon + 1;
        } else {
            /* Last field or newline stripping */
            char *nl = strchr(cur, '\n');
            if (nl) *nl = '\0';
            char *cr = strchr(cur, '\r');
            if (cr) *cr = '\0';
            if (i < 6) return NULL;
        }
    }

    s_pw_buf.pw_name   = fields[0];
    s_pw_buf.pw_passwd = fields[1];
    s_pw_buf.pw_uid    = (uid_t)atoi(fields[2]);
    s_pw_buf.pw_gid    = (gid_t)atoi(fields[3]);
    s_pw_buf.pw_gecos  = fields[4];
    s_pw_buf.pw_dir    = fields[5];
    s_pw_buf.pw_shell  = fields[6];

    return &s_pw_buf;
}

struct passwd *getpwent(void)
{
    if (!s_pw_file) {
        setpwent();
        if (!s_pw_file) return NULL;
    }

    while (fgets(s_line_buf, sizeof(s_line_buf), s_pw_file)) {
        if (s_line_buf[0] == '#' || s_line_buf[0] == '\n' || s_line_buf[0] == '\r') continue;
        struct passwd *pw = parse_passwd_line(s_line_buf);
        if (pw) return pw;
    }
    return NULL;
}

struct passwd *getpwnam(const char *name)
{
    if (!name) return NULL;
    setpwent();
    struct passwd *pw;
    while ((pw = getpwent()) != NULL) {
        if (strcmp(pw->pw_name, name) == 0) {
            endpwent();
            return pw;
        }
    }
    endpwent();

    /* Fallback default root user */
    if (strcmp(name, "root") == 0) {
        s_pw_buf.pw_name   = "root";
        s_pw_buf.pw_passwd = "x";
        s_pw_buf.pw_uid    = 0;
        s_pw_buf.pw_gid    = 0;
        s_pw_buf.pw_gecos  = "root";
        s_pw_buf.pw_dir    = "/root";
        s_pw_buf.pw_shell  = "/bin/sh.elf";
        return &s_pw_buf;
    }
    return NULL;
}

struct passwd *getpwuid(uid_t uid)
{
    setpwent();
    struct passwd *pw;
    while ((pw = getpwent()) != NULL) {
        if (pw->pw_uid == uid) {
            endpwent();
            return pw;
        }
    }
    endpwent();

    /* Fallback default for uid 0 */
    if (uid == 0) {
        s_pw_buf.pw_name   = "root";
        s_pw_buf.pw_passwd = "x";
        s_pw_buf.pw_uid    = 0;
        s_pw_buf.pw_gid    = 0;
        s_pw_buf.pw_gecos  = "root";
        s_pw_buf.pw_dir    = "/root";
        s_pw_buf.pw_shell  = "/bin/sh.elf";
        return &s_pw_buf;
    }
    return NULL;
}
