/* ============================================================================
 * AzamiOS Userspace — POSIX Identity Utility (id.elf)
 * File: userland/apps/id/main.c
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/string.h"
#include "../../libc/include/unistd.h"
#include "../../libc/include/pwd.h"
#include "../../libc/include/grp.h"

int main(int argc, char **argv)
{
    int opt_u = 0, opt_g = 0, opt_G = 0, opt_n = 0, opt_r = 0;

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            for (int j = 1; argv[i][j]; j++) {
                if (argv[i][j] == 'u') opt_u = 1;
                else if (argv[i][j] == 'g') opt_g = 1;
                else if (argv[i][j] == 'G') opt_G = 1;
                else if (argv[i][j] == 'n') opt_n = 1;
                else if (argv[i][j] == 'r') opt_r = 1;
            }
        }
    }

    uid_t uid = opt_r ? getuid() : geteuid();
    gid_t gid = opt_r ? getgid() : getegid();

    struct passwd *pw = getpwuid(uid);
    const char *uname = pw ? pw->pw_name : "unknown";

    struct group *gr = getgrgid(gid);
    const char *gname = gr ? gr->gr_name : "unknown";

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
        if (opt_n) printf("%s\n", gname);
        else printf("%u\n", gid);
        return 0;
    }

    /* Standard default POSIX output */
    printf("uid=%u(%s) gid=%u(%s) groups=%u(%s)\n",
           uid, uname, gid, gname, gid, gname);

    return 0;
}
