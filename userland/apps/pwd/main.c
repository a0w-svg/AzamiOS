/* ============================================================================
 * AzamiOS Userspace — Print Working Directory (pwd.elf)
 * File: userland/apps/pwd/main.c
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/string.h"
#include "../../libc/include/unistd.h"
#include "../../libc/include/getopt.h"
#include "../../libc/include/sys/stat.h"

static void print_help(void)
{
    printf("Usage: pwd [-L | -P]\n"
           "Print the name of the current working directory.\n\n"
           "  -L, --logical   use PWD from environment, even if it contains symlinks\n"
           "  -P, --physical  avoid all symlinks\n"
           "      --help      display this help and exit\n"
           "      --version   output version information and exit\n");
}

static void print_version(void)
{
    printf("pwd (AzamiOS coreutils) 7.0.0\n");
}

int main(int argc, char **argv)
{
    int logical = 1; /* Default is logical in standard shell environments */

    static struct option long_options[] = {
        {"logical",  no_argument, 0, 'L'},
        {"physical", no_argument, 0, 'P'},
        {"help",     no_argument, 0, 'h'},
        {"version",  no_argument, 0, 'v'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "LPhv", long_options, NULL)) != -1) {
        switch (opt) {
        case 'L':
            logical = 1;
            break;
        case 'P':
            logical = 0;
            break;
        case 'h':
            print_help();
            return 0;
        case 'v':
            print_version();
            return 0;
        default:
            fprintf(stderr, "Try 'pwd --help' for more information.\n");
            return 1;
        }
    }

    if (logical) {
        const char *pwd_env = getenv("PWD");
        if (pwd_env && pwd_env[0] == '/') {
            struct stat st_env, st_cur;
            if (stat(pwd_env, &st_env) == 0 && stat(".", &st_cur) == 0) {
                if (st_env.st_ino == st_cur.st_ino && st_env.st_dev == st_cur.st_dev) {
                    printf("%s\n", pwd_env);
                    return 0;
                }
            }
        }
    }

    char cwd[1024];
    if (getcwd(cwd, sizeof(cwd))) {
        printf("%s\n", cwd);
        return 0;
    } else {
        perror("pwd");
        return 1;
    }
}
