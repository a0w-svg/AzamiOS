/* ============================================================================
 * AzamiOS Userspace — System Name (uname.elf)
 * File: userland/apps/uname/main.c
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/string.h"
#include "../../libc/include/stdbool.h"
#include "../../libc/include/sys/utsname.h"

static void print_usage(const char *prog)
{
    printf("Usage: %s [OPTION]...\n"
           "Print certain system information. With no OPTION, same as -s.\n\n"
           "  -a, --all                print all information\n"
           "  -s, --kernel-name        print the kernel name\n"
           "  -n, --nodename           print the network node hostname\n"
           "  -r, --kernel-release     print the kernel release\n"
           "  -v, --kernel-version     print the kernel version\n"
           "  -m, --machine            print the machine hardware name\n"
           "  -p, --processor          print the processor type\n"
           "  -i, --hardware-platform  print the hardware platform\n"
           "  -o, --operating-system   print the operating system\n"
           "      --help     display this help and exit\n"
           "      --version  output version information and exit\n",
           prog);
}

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
    bool processor = false;
    bool platform = false;
    bool os = false;

    if (argc <= 1) {
        sysname = true;
    } else {
        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "--help") == 0) {
                print_usage(argv[0]);
                return 0;
            }
            if (strcmp(argv[i], "--version") == 0) {
                printf("uname (AzamiOS coreutils) 7.0\n");
                return 0;
            }
            if (strcmp(argv[i], "--all") == 0) {
                all = true;
                continue;
            }
            if (strcmp(argv[i], "--kernel-name") == 0) { sysname = true; continue; }
            if (strcmp(argv[i], "--nodename") == 0) { nodename = true; continue; }
            if (strcmp(argv[i], "--kernel-release") == 0) { release = true; continue; }
            if (strcmp(argv[i], "--kernel-version") == 0) { version = true; continue; }
            if (strcmp(argv[i], "--machine") == 0) { machine = true; continue; }
            if (strcmp(argv[i], "--processor") == 0) { processor = true; continue; }
            if (strcmp(argv[i], "--hardware-platform") == 0) { platform = true; continue; }
            if (strcmp(argv[i], "--operating-system") == 0) { os = true; continue; }

            if (argv[i][0] == '-') {
                for (int j = 1; argv[i][j] != '\0'; j++) {
                    switch (argv[i][j]) {
                        case 'a': all = true; break;
                        case 's': sysname = true; break;
                        case 'n': nodename = true; break;
                        case 'r': release = true; break;
                        case 'v': version = true; break;
                        case 'm': machine = true; break;
                        case 'p': processor = true; break;
                        case 'i': platform = true; break;
                        case 'o': os = true; break;
                        default:
                            fprintf(stderr, "uname: invalid option -- '%c'\n"
                                            "Try 'uname --help' for more information.\n", argv[i][j]);
                            return 1;
                    }
                }
            } else {
                fprintf(stderr, "uname: extra operand '%s'\n"
                                "Try 'uname --help' for more information.\n", argv[i]);
                return 1;
            }
        }
    }

    const char *proc_type = u.machine[0] ? u.machine : "x86_64";
    const char *plat_type = u.machine[0] ? u.machine : "x86_64";
    const char *os_name = "AzamiOS";

    if (all) {
        printf("%s %s %s %s %s %s %s %s\n",
               u.sysname, u.nodename, u.release, u.version, u.machine,
               proc_type, plat_type, os_name);
        return 0;
    }

    bool first = true;
    if (sysname)   { printf("%s%s", first ? "" : " ", u.sysname); first = false; }
    if (nodename)  { printf("%s%s", first ? "" : " ", u.nodename); first = false; }
    if (release)   { printf("%s%s", first ? "" : " ", u.release); first = false; }
    if (version)   { printf("%s%s", first ? "" : " ", u.version); first = false; }
    if (machine)   { printf("%s%s", first ? "" : " ", u.machine); first = false; }
    if (processor) { printf("%s%s", first ? "" : " ", proc_type); first = false; }
    if (platform)  { printf("%s%s", first ? "" : " ", plat_type); first = false; }
    if (os)        { printf("%s%s", first ? "" : " ", os_name); first = false; }
    putchar('\n');

    return 0;
}
