/* ============================================================================
 * AzamiOS Userspace — POSIX nice Utility (main.c)
 * File: userland/apps/nice/main.c
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/resource.h>
#include <errno.h>

static void print_help(void)
{
    printf("Usage: nice [OPTION] [COMMAND [ARG]...]\n"
           "Run COMMAND with an adjusted niceness, which affects process scheduling.\n"
           "With no COMMAND, print the current niceness.\n\n"
           "  -n, --adjustment=N   add integer N to the niceness (default 10)\n"
           "      --help           display this help and exit\n");
}

int main(int argc, char *argv[])
{
    int adjustment = 10;
    int arg_idx = 1;

    if (argc > 1 && strcmp(argv[1], "--help") == 0) {
        print_help();
        return 0;
    }

    if (argc > 1 && (strcmp(argv[1], "-n") == 0 || strncmp(argv[1], "--adjustment=", 13) == 0)) {
        if (strcmp(argv[1], "-n") == 0) {
            if (argc < 3) {
                fprintf(stderr, "nice: option requires an argument -- 'n'\n");
                return 1;
            }
            adjustment = atoi(argv[2]);
            arg_idx = 3;
        } else {
            adjustment = atoi(argv[1] + 13);
            arg_idx = 2;
        }
    } else if (argc > 1 && argv[1][0] == '-' && argv[1][1] >= '0' && argv[1][1] <= '9') {
        adjustment = atoi(argv[1] + 1);
        arg_idx = 2;
    }

    if (arg_idx >= argc) {
        errno = 0;
        int current_nice = getpriority(PRIO_PROCESS, 0);
        printf("%d\n", current_nice);
        return 0;
    }

    if (nice(adjustment) == -1 && errno != 0) {
        perror("nice");
    }

    execvp(argv[arg_idx], &argv[arg_idx]);
    perror(argv[arg_idx]);
    return 127;
}
