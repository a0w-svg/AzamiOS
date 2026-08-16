/* ============================================================================
 * AzamiOS Userspace — System Name (uname.elf)
 * File: userland/apps/uname/main.c
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/string.h"
#include "../../libc/include/stdbool.h"
#include "../../libc/include/sys/utsname.h"

int main(int argc, char **argv)
{
    struct utsname u;
    if (uname(&u) != 0) {
        printf("uname: syscall failed\n");
        return 1;
    }

    bool all = false;
    bool sysname = false;
    bool nodename = false;
    bool release = false;
    bool version = false;
    bool machine = false;

    if (argc <= 1) {
        sysname = true;
    } else {
        for (int i = 1; i < argc; i++) {
            if (argv[i][0] == '-') {
                for (int j = 1; argv[i][j] != '\0'; j++) {
                    switch (argv[i][j]) {
                        case 'a': all = true; break;
                        case 's': sysname = true; break;
                        case 'n': nodename = true; break;
                        case 'r': release = true; break;
                        case 'v': version = true; break;
                        case 'm': machine = true; break;
                        default: break;
                    }
                }
            }
        }
    }

    if (all) {
        printf("%s %s %s %s %s\n", u.sysname, u.nodename, u.release, u.version, u.machine);
        return 0;
    }

    bool first = true;
    if (sysname) { printf("%s%s", first ? "" : " ", u.sysname); first = false; }
    if (nodename) { printf("%s%s", first ? "" : " ", u.nodename); first = false; }
    if (release) { printf("%s%s", first ? "" : " ", u.release); first = false; }
    if (version) { printf("%s%s", first ? "" : " ", u.version); first = false; }
    if (machine) { printf("%s%s", first ? "" : " ", u.machine); first = false; }
    putchar('\n');

    return 0;
}
