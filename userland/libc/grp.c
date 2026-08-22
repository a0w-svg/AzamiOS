/* ============================================================================
 * AzamiOS Userspace — Group Database Implementation (grp.c)
 * File: userland/libc/grp.c
 * ============================================================================ */

#include "include/grp.h"
#include "include/stdio.h"
#include "include/stdlib.h"
#include "include/string.h"
#include "include/unistd.h"

static FILE *s_gr_file = NULL;
static struct group s_gr_buf;
static char s_gr_line[256];
static char *s_gr_mem[16];

void setgrent(void)
{
    if (s_gr_file) {
        rewind(s_gr_file);
    } else {
        s_gr_file = fopen("/etc/group", "r");
    }
}

void endgrent(void)
{
    if (s_gr_file) {
        fclose(s_gr_file);
        s_gr_file = NULL;
    }
}

static struct group *parse_group_line(char *line)
{
    if (!line) return NULL;
    /* Format: group_name:password:gid:user1,user2,... */
    char *fields[4];
    char *cur = line;
    for (int i = 0; i < 4; i++) {
        fields[i] = cur;
        char *colon = strchr(cur, ':');
        if (colon) {
            *colon = '\0';
            cur = colon + 1;
        } else {
            char *nl = strchr(cur, '\n');
            if (nl) *nl = '\0';
            char *cr = strchr(cur, '\r');
            if (cr) *cr = '\0';
            if (i < 3) return NULL;
        }
    }

    s_gr_buf.gr_name   = fields[0];
    s_gr_buf.gr_passwd = fields[1];
    s_gr_buf.gr_gid    = (gid_t)atoi(fields[2]);

    /* Parse comma-separated members */
    char *mcur = fields[3];
    int mi = 0;
    while (mcur && *mcur && mi < 15) {
        char *comma = strchr(mcur, ',');
        if (comma) {
            *comma = '\0';
            s_gr_mem[mi++] = mcur;
            mcur = comma + 1;
        } else {
            s_gr_mem[mi++] = mcur;
            break;
        }
    }
    s_gr_mem[mi] = NULL;
    s_gr_buf.gr_mem = s_gr_mem;

    return &s_gr_buf;
}

struct group *getgrent(void)
{
    if (!s_gr_file) {
        setgrent();
        if (!s_gr_file) return NULL;
    }

    while (fgets(s_gr_line, sizeof(s_gr_line), s_gr_file)) {
        if (s_gr_line[0] == '#' || s_gr_line[0] == '\n' || s_gr_line[0] == '\r') continue;
        struct group *gr = parse_group_line(s_gr_line);
        if (gr) return gr;
    }
    return NULL;
}

struct group *getgrnam(const char *name)
{
    if (!name) return NULL;
    setgrent();
    struct group *gr;
    while ((gr = getgrent()) != NULL) {
        if (strcmp(gr->gr_name, name) == 0) {
            endgrent();
            return gr;
        }
    }
    endgrent();

    /* Fallback default root group */
    if (strcmp(name, "root") == 0 || strcmp(name, "wheel") == 0) {
        s_gr_buf.gr_name   = (char *)name;
        s_gr_buf.gr_passwd = "x";
        s_gr_buf.gr_gid    = 0;
        s_gr_mem[0]        = "root";
        s_gr_mem[1]        = NULL;
        s_gr_buf.gr_mem    = s_gr_mem;
        return &s_gr_buf;
    }
    return NULL;
}

struct group *getgrgid(gid_t gid)
{
    setgrent();
    struct group *gr;
    while ((gr = getgrent()) != NULL) {
        if (gr->gr_gid == gid) {
            endgrent();
            return gr;
        }
    }
    endgrent();

    if (gid == 0) {
        s_gr_buf.gr_name   = "root";
        s_gr_buf.gr_passwd = "x";
        s_gr_buf.gr_gid    = 0;
        s_gr_mem[0]        = "root";
        s_gr_mem[1]        = NULL;
        s_gr_buf.gr_mem    = s_gr_mem;
        return &s_gr_buf;
    }
    return NULL;
}
