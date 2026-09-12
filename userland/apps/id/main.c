/* ============================================================================
 * AzamiOS Userspace — POSIX Identity Utility (id.elf)
 * File: userland/apps/id/main.c
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/string.h"
#include "../../libc/include/stdbool.h"
#include "../../libc/include/unistd.h"
#include "../../libc/include/pwd.h"
#include "../../libc/include/grp.h"

static void print_usage(const char *prog)
{
    printf("Usage: %s [OPTION]... [USER]\n"
           "Print user and group information for each specified USER,\n"
           "or (when USER omitted) for the current process.\n\n"
           "  -g, --group     print only the effective group ID\n"
           "  -G, --groups    print all group IDs\n"
           "  -n, --name      print a name instead of a number, for -ugG\n"
           "  -r, --real      print the real ID instead of the effective ID, with -ugG\n"
           "  -u, --user      print only the effective user ID\n"
           "      --help      display this help and exit\n"
           "      --version   output version information and exit\n",
           prog);
}

typedef struct {
    gid_t gid;
    char name[32];
} group_entry_info_t;

#define MAX_SUPP_GROUPS 32

static int get_user_groups(const char *username, gid_t primary_gid, group_entry_info_t *out_groups, int max_groups)
{
    int count = 0;

    /* Add primary group first */
    struct group *pgr = getgrgid(primary_gid);
    out_groups[count].gid = primary_gid;
    if (pgr && pgr->gr_name) {
        strncpy(out_groups[count].name, pgr->gr_name, sizeof(out_groups[count].name) - 1);
        out_groups[count].name[sizeof(out_groups[count].name) - 1] = '\0';
    } else {
        snprintf(out_groups[count].name, sizeof(out_groups[count].name), "%u", primary_gid);
    }
    count++;

    /* Enumerate all groups from /etc/group */
    setgrent();
    struct group *gr;
    while ((gr = getgrent()) != NULL && count < max_groups) {
        if (gr->gr_gid == primary_gid) continue; /* already added */

        bool is_member = false;
        if (gr->gr_mem) {
            for (char **m = gr->gr_mem; *m; m++) {
                if (strcmp(*m, username) == 0) {
                    is_member = true;
                    break;
                }
            }
        }

        if (is_member) {
            out_groups[count].gid = gr->gr_gid;
            strncpy(out_groups[count].name, gr->gr_name, sizeof(out_groups[count].name) - 1);
            out_groups[count].name[sizeof(out_groups[count].name) - 1] = '\0';
            count++;
        }
    }
    endgrent();

    return count;
}

int main(int argc, char **argv)
{
    int opt_u = 0, opt_g = 0, opt_G = 0, opt_n = 0, opt_r = 0;
    const char *target_user = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        if (strcmp(argv[i], "--version") == 0) {
            printf("id (AzamiOS coreutils) 7.0\n");
            return 0;
        }
        if (strcmp(argv[i], "--user") == 0)   { opt_u = 1; continue; }
        if (strcmp(argv[i], "--group") == 0)  { opt_g = 1; continue; }
        if (strcmp(argv[i], "--groups") == 0) { opt_G = 1; continue; }
        if (strcmp(argv[i], "--name") == 0)   { opt_n = 1; continue; }
        if (strcmp(argv[i], "--real") == 0)   { opt_r = 1; continue; }

        if (argv[i][0] == '-') {
            for (int j = 1; argv[i][j]; j++) {
                if (argv[i][j] == 'u') opt_u = 1;
                else if (argv[i][j] == 'g') opt_g = 1;
                else if (argv[i][j] == 'G') opt_G = 1;
                else if (argv[i][j] == 'n') opt_n = 1;
                else if (argv[i][j] == 'r') opt_r = 1;
                else {
                    fprintf(stderr, "id: invalid option -- '%c'\n", argv[i][j]);
                    return 1;
                }
            }
        } else {
            target_user = argv[i];
        }
    }

    uid_t uid;
    gid_t gid;
    const char *uname = NULL;
    char uname_buf[32];

    if (target_user) {
        struct passwd *pw = getpwnam(target_user);
        if (!pw) {
            /* Try numeric parse */
            char *endp = NULL;
            unsigned long num_uid = strtoul(target_user, &endp, 10);
            if (endp && *endp == '\0') {
                pw = getpwuid((uid_t)num_uid);
            }
        }
        if (!pw) {
            fprintf(stderr, "id: '%s': no such user\n", target_user);
            return 1;
        }
        uid = pw->pw_uid;
        gid = pw->pw_gid;
        strncpy(uname_buf, pw->pw_name, sizeof(uname_buf) - 1);
        uname_buf[sizeof(uname_buf) - 1] = '\0';
        uname = uname_buf;
    } else {
        uid = opt_r ? getuid() : geteuid();
        gid = opt_r ? getgid() : getegid();
        struct passwd *pw = getpwuid(uid);
        if (pw) {
            strncpy(uname_buf, pw->pw_name, sizeof(uname_buf) - 1);
            uname_buf[sizeof(uname_buf) - 1] = '\0';
            uname = uname_buf;
        } else {
            uname = "unknown";
        }
    }

    struct group *gr = getgrgid(gid);
    const char *gname = gr ? gr->gr_name : "unknown";

    group_entry_info_t groups[MAX_SUPP_GROUPS];
    int num_groups = get_user_groups(uname, gid, groups, MAX_SUPP_GROUPS);

    if (opt_u) {
        if (opt_n) printf("%s\n", uname);
        else printf("%u\n", uid);
        return 0;
    }

    if (opt_g) {
        if (opt_n) printf("%s\n", gname);
        else printf("%u\n", gid);
        return 0;
    }

    if (opt_G) {
        for (int i = 0; i < num_groups; i++) {
            if (i > 0) putchar(' ');
            if (opt_n) printf("%s", groups[i].name);
            else printf("%u", groups[i].gid);
        }
        putchar('\n');
        return 0;
    }

    /* Standard default POSIX output */
    printf("uid=%u(%s) gid=%u(%s) groups=", uid, uname, gid, gname);
    for (int i = 0; i < num_groups; i++) {
        if (i > 0) putchar(',');
        printf("%u(%s)", groups[i].gid, groups[i].name);
    }
    putchar('\n');

    return 0;
}
