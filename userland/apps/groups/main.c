/* ============================================================================
 * AzamiOS Userspace — POSIX groups Utility (main.c)
 * File: userland/apps/groups/main.c
 * ============================================================================ */
#include <stdio.h>
#include <unistd.h>
#include <grp.h>
#include <pwd.h>

int main(int argc, char *argv[])
{
    gid_t groups[32];
    int ngroups = 0;

    if (argc <= 1) {
        ngroups = getgroups(32, groups);
        if (ngroups < 0) ngroups = 0;
        gid_t egid = getegid();
        struct group *gr = getgrgid(egid);
        if (gr) printf("%s", gr->gr_name);
        else printf("%u", (unsigned int)egid);

        for (int i = 0; i < ngroups; i++) {
            if (groups[i] == egid) continue;
            gr = getgrgid(groups[i]);
            if (gr) printf(" %s", gr->gr_name);
            else printf(" %u", (unsigned int)groups[i]);
        }
        printf("\n");
        return 0;
    }

    for (int i = 1; i < argc; i++) {
        struct passwd *pw = getpwnam(argv[i]);
        if (!pw) {
            fprintf(stderr, "groups: '%s': no such user\n", argv[i]);
            continue;
        }
        printf("%s : ", argv[i]);
        struct group *gr = getgrgid(pw->pw_gid);
        if (gr) printf("%s", gr->gr_name);
        else printf("%u", (unsigned int)pw->pw_gid);
        printf("\n");
    }
    return 0;
}
