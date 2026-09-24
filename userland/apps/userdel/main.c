/* ============================================================================
 * AzamiOS Userspace — Account Removal Utility (userdel.elf)
 * File: userland/apps/userdel/main.c
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pwd.h>

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

/* Removes every line starting with "name:" from `buf` (colon-delimited
 * account files -- passwd/shadow/group all use this same record shape).
 * Returns 1 if a line was actually removed. */
static int remove_entry(char *buf, const char *name)
{
    char prefix[128];
    snprintf(prefix, sizeof(prefix), "%s:", name);
    size_t plen = strlen(prefix);
    int removed = 0;
    char *p = buf;
    while (*p) {
        char *nl = strchr(p, '\n');
        size_t linelen = nl ? (size_t)(nl - p) + 1 : strlen(p);
        if (strncmp(p, prefix, plen) == 0) {
            memmove(p, p + linelen, strlen(p + linelen) + 1);
            removed = 1;
        } else {
            p += linelen;
        }
    }
    return removed;
}

int main(int argc, char **argv)
{
    if (geteuid() != 0) {
        fprintf(stderr, "userdel: only root can remove accounts\n");
        return 1;
    }

    const char *username = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("Usage: %s <username>\n"
                   "Removes the account's /etc/passwd, /etc/shadow, and matching\n"
                   "private-group /etc/group entries. Must be run as root.\n", argv[0]);
            return 0;
        }
        username = argv[i];
    }
    if (!username) {
        fprintf(stderr, "Usage: %s <username>\n", argv[0]);
        return 1;
    }
    if (strcmp(username, "root") == 0) {
        fprintf(stderr, "userdel: refusing to remove 'root'\n");
        return 1;
    }

    struct passwd *pw = getpwnam(username);
    if (!pw) {
        fprintf(stderr, "userdel: user '%s' does not exist\n", username);
        return 1;
    }
    uid_t victim_uid = pw->pw_uid;
    gid_t victim_gid = pw->pw_gid;

    char passwd_buf[MAX_FILE], shadow_buf[MAX_FILE], group_buf[MAX_FILE];
    read_whole_file("/etc/passwd", passwd_buf, sizeof(passwd_buf));
    read_whole_file("/etc/shadow", shadow_buf, sizeof(shadow_buf));
    read_whole_file("/etc/group",  group_buf,  sizeof(group_buf));

    remove_entry(passwd_buf, username);
    remove_entry(shadow_buf, username);

    /* Only drop the private group useradd created (name == username, gid
     * == the account's own gid) -- a supplementary group the account was
     * merely added to (wheel, users, ...) is shared and stays, just like
     * real userdel behaves without --force on a group other users belong
     * to. */
    char pgroup_prefix[160];
    snprintf(pgroup_prefix, sizeof(pgroup_prefix), "%s:x:%u:", username, (unsigned)victim_gid);
    if (strstr(group_buf, pgroup_prefix)) {
        remove_entry(group_buf, username);
    }
    (void)victim_uid;

    if (write_whole_file("/etc/passwd", passwd_buf, 0644) != 0 ||
        write_whole_file("/etc/shadow", shadow_buf, 0600) != 0 ||
        write_whole_file("/etc/group",  group_buf,  0644) != 0) {
        fprintf(stderr, "userdel: failed to write account database\n");
        return 1;
    }

    /* Best-effort: only removes the home directory if it's already empty,
     * matching userdel's behavior without -r -- deleting a populated home
     * directory is destructive enough that it shouldn't happen implicitly. */
    if (pw->pw_dir && pw->pw_dir[0]) {
        if (rmdir(pw->pw_dir) != 0) {
            printf("userdel: '%s' removed (home directory %s left in place)\n",
                   username, pw->pw_dir);
            return 0;
        }
    }

    printf("userdel: '%s' removed\n", username);
    return 0;
}
