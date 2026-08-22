/* ============================================================================
 * AzamiOS Userspace — WhoAmI & ID Utility (whoami.elf)
 * File: userland/apps/whoami/main.c
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/string.h"
#include "../../libc/include/unistd.h"
#include "../../libc/include/sys/syscall.h"

int main(int argc, char **argv)
{
    uid_t uid = getuid();
    gid_t gid = getgid();

    if (argc > 1 && (strcmp(argv[1], "-a") == 0 || strcmp(argv[1], "--all") == 0 || strcmp(argv[1], "-u") == 0)) {
        printf("uid=%u(%s) gid=%u(%s) groups=%u(%s)\n",
               uid, (uid == 0 ? "root" : "azami"),
               gid, (gid == 0 ? "root" : "users"),
               gid, (gid == 0 ? "root" : "users"));
        return 0;
    }

    if (argc > 1 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        puts("Usage: whoami [OPTION]");
        puts("Print the current effective username.");
        puts("  -a, --all    Print full UID/GID details");
        return 0;
    }

    if (uid == 0) {
        puts("root");
    } else if (uid == 1000) {
        puts("azami");
    } else {
        printf("user%u\n", uid);
    }

    return 0;
}
