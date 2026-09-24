/* ============================================================================
 * AzamiOS Userspace — Account Creation Utility (useradd.elf)
 * File: userland/apps/useradd/main.c
 *
 * Wires up real account creation against the /etc/passwd, /etc/shadow, and
 * /etc/group infrastructure this OS already has (real SHA-256 password
 * verification in lockscreen.elf, real setuid/setgid/setgroups kernel
 * support, real per-file permission enforcement) -- until now there was no
 * way to actually add a user to any of it after the two accounts the build
 * ships with.
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <pwd.h>
#include <grp.h>
#include <sha256.h>

#define MAX_FILE 8192

static int read_whole_file(const char *path, char *buf, size_t max_len)
{
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) { buf[0] = '\0'; return 0; }
    ssize_t n = read(fd, buf, max_len - 1);
    close(fd);
    if (n < 0) n = 0;
    buf[n] = '\0';
    return (int)n;
}

static int write_whole_file(const char *path, const char *buf, int mode)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, (unsigned)mode);
    if (fd < 0) return -1;
    size_t len = strlen(buf);
    ssize_t w = write(fd, buf, len);
    close(fd);
    return (w == (ssize_t)len) ? 0 : -1;
}

/* True if any line in `buf` (colon-separated) starts with "name:". */
static int has_entry(const char *buf, const char *name)
{
    char prefix[128];
    snprintf(prefix, sizeof(prefix), "%s:", name);
    size_t plen = strlen(prefix);
    const char *p = buf;
    while (*p) {
        if (strncmp(p, prefix, plen) == 0) return 1;
        const char *nl = strchr(p, '\n');
        if (!nl) break;
        p = nl + 1;
    }
    return 0;
}

/* Scans /etc/passwd for the highest uid >= 1000 and returns one past it,
 * the same "next free normal-user id" convention real useradd uses. */
static int next_free_uid(void)
{
    int max_uid = 999;
    setpwent();
    struct passwd *pw;
    while ((pw = getpwent()) != NULL) {
        if ((int)pw->pw_uid > max_uid && pw->pw_uid < 60000) max_uid = (int)pw->pw_uid;
    }
    endpwent();
    return max_uid + 1;
}

static void print_usage(const char *prog)
{
    printf("Usage: %s [-u uid] [-g gid] [-G group1,group2] [-d home] [-s shell]\n"
           "                [-c comment] [-p password] <username>\n\n"
           "Creates a real account: appends /etc/passwd, /etc/shadow, /etc/group\n"
           "entries and creates the home directory, owned by the new account.\n"
           "Without -p the account is created locked (no password login) --\n"
           "run 'passwd <username>' afterward to set one.\n"
           "Must be run as root.\n", prog);
}

int main(int argc, char **argv)
{
    if (geteuid() != 0) {
        fprintf(stderr, "useradd: only root can create accounts\n");
        return 1;
    }

    int uid = -1, gid = -1;
    const char *groups_csv = NULL;
    const char *home = NULL;
    const char *shell = "/bin/bash";
    const char *comment = "";
    const char *password = NULL;
    const char *username = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "-u") == 0 && i + 1 < argc) {
            uid = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-g") == 0 && i + 1 < argc) {
            gid = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-G") == 0 && i + 1 < argc) {
            groups_csv = argv[++i];
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            home = argv[++i];
        } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            shell = argv[++i];
        } else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            comment = argv[++i];
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            password = argv[++i];
        } else if (argv[i][0] != '-') {
            username = argv[i];
        } else {
            fprintf(stderr, "useradd: unrecognized option '%s'\n", argv[i]);
            return 1;
        }
    }

    if (!username || !username[0]) {
        print_usage(argv[0]);
        return 1;
    }
    for (const char *p = username; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')) {
            fprintf(stderr, "useradd: invalid username '%s' (lowercase letters, digits, "
                            "'-', '_' only)\n", username);
            return 1;
        }
    }

    char passwd_buf[MAX_FILE], shadow_buf[MAX_FILE], group_buf[MAX_FILE];
    read_whole_file("/etc/passwd", passwd_buf, sizeof(passwd_buf));
    read_whole_file("/etc/shadow", shadow_buf, sizeof(shadow_buf));
    read_whole_file("/etc/group",  group_buf,  sizeof(group_buf));

    if (has_entry(passwd_buf, username)) {
        fprintf(stderr, "useradd: user '%s' already exists\n", username);
        return 1;
    }

    if (uid < 0) uid = next_free_uid();
    if (gid < 0) gid = uid; /* private per-user group, matching the uid */

    char home_buf[128];
    if (!home) {
        snprintf(home_buf, sizeof(home_buf), "/home/%s", username);
        home = home_buf;
    }

    /* /etc/passwd: username:x:uid:gid:comment:home:shell */
    char passwd_line[384];
    snprintf(passwd_line, sizeof(passwd_line), "%s:x:%d:%d:%s:%s:%s\n",
             username, uid, gid, comment, home, shell);
    strncat(passwd_buf, passwd_line, sizeof(passwd_buf) - strlen(passwd_buf) - 1);

    /* /etc/shadow: real salted SHA-256 if -p was given, otherwise the
     * account is locked ("!") -- same convention real useradd uses when no
     * password is supplied on the command line. */
    char shadow_hash[256];
    if (password && password[0]) {
        sha256_make_shadow_entry(password, 16, shadow_hash, sizeof(shadow_hash));
    } else {
        strcpy(shadow_hash, "!");
    }
    char shadow_line[320];
    snprintf(shadow_line, sizeof(shadow_line), "%s:%s:19000:0:99999:7:::\n", username, shadow_hash);
    strncat(shadow_buf, shadow_line, sizeof(shadow_buf) - strlen(shadow_buf) - 1);

    /* /etc/group: a private group matching the new uid/gid, unless an
     * existing group already owns that gid (e.g. -g reused "users"). */
    char gid_prefix[32];
    snprintf(gid_prefix, sizeof(gid_prefix), ":x:%d:", gid);
    if (!strstr(group_buf, gid_prefix)) {
        char group_line[192];
        snprintf(group_line, sizeof(group_line), "%s:x:%d:\n", username, gid);
        strncat(group_buf, group_line, sizeof(group_buf) - strlen(group_buf) - 1);
    }

    /* Supplementary groups (-G): append the username to each named group's
     * member list in place. */
    if (groups_csv) {
        char csv_copy[256];
        strncpy(csv_copy, groups_csv, sizeof(csv_copy) - 1);
        csv_copy[sizeof(csv_copy) - 1] = '\0';
        char *tok = strtok(csv_copy, ",");
        while (tok) {
            char gprefix[128];
            snprintf(gprefix, sizeof(gprefix), "%s:x:", tok);
            char *line = strstr(group_buf, gprefix);
            if (line) {
                char *nl = strchr(line, '\n');
                size_t line_len = nl ? (size_t)(nl - line) : strlen(line);
                char newline_buf[MAX_FILE];
                size_t prefix_len = (size_t)(line - group_buf) + line_len;
                memcpy(newline_buf, group_buf, prefix_len);
                int add_comma = (line[line_len - 1] != ':');
                int extra = snprintf(newline_buf + prefix_len, sizeof(newline_buf) - prefix_len,
                                      "%s%s", add_comma ? "," : "", username);
                snprintf(newline_buf + prefix_len + extra, sizeof(newline_buf) - prefix_len - (size_t)extra,
                         "%s", nl ? nl : "");
                strncpy(group_buf, newline_buf, sizeof(group_buf) - 1);
                group_buf[sizeof(group_buf) - 1] = '\0';
            } else {
                fprintf(stderr, "useradd: warning: group '%s' does not exist, skipping\n", tok);
            }
            tok = strtok(NULL, ",");
        }
    }

    if (write_whole_file("/etc/passwd", passwd_buf, 0644) != 0 ||
        write_whole_file("/etc/shadow", shadow_buf, 0600) != 0 ||
        write_whole_file("/etc/group",  group_buf,  0644) != 0) {
        fprintf(stderr, "useradd: failed to write account database\n");
        return 1;
    }

    /* Home directory, owned by the new account -- not root, so the real
     * permission enforcement now wired into open()/openat() actually lets
     * the new user in. */
    mkdir(home, 0700);
    chown(home, (uid_t)uid, (gid_t)gid);

    printf("useradd: created '%s' (uid=%d, gid=%d, home=%s)%s\n",
           username, uid, gid, home,
           (password && password[0]) ? "" : " [locked -- run 'passwd' to set a password]");
    return 0;
}
