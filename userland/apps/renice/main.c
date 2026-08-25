/* ============================================================================
 * AzamiOS Userspace — POSIX renice Utility (main.c)
 * File: userland/apps/renice/main.c
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/resource.h>
#include <pwd.h>
#include <errno.h>

static void print_help(void)
{
    printf("Usage: renice [-n] PRIORITY [[-p] PID...] [[-g] PGRP...] [[-u] USER...]\n"
           "Alter priority of running processes.\n\n"
           "  -n, --priority PRIORITY  specify the nice value\n"
           "  -p, --pid                interpret arguments as process ID (default)\n"
           "  -g, --pgrp               interpret arguments as process group ID\n"
           "  -u, --user               interpret arguments as user name or ID\n"
           "  -h, --help               display this help and exit\n");
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "renice: missing operand\nTry 'renice --help' for more information.\n");
        return 1;
    }

    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        print_help();
        return 0;
    }

    int priority = 0;
    int which = PRIO_PROCESS;
    int arg_idx = 1;

    if (strcmp(argv[1], "-n") == 0) {
        if (argc < 3) {
            fprintf(stderr, "renice: option requires an argument -- 'n'\n");
            return 1;
        }
        priority = atoi(argv[2]);
        arg_idx = 3;
    } else {
        priority = atoi(argv[1]);
        arg_idx = 2;
    }

    int ret = 0;
    for (int i = arg_idx; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--pid") == 0) {
            which = PRIO_PROCESS;
            continue;
        }
        if (strcmp(argv[i], "-g") == 0 || strcmp(argv[i], "--pgrp") == 0) {
            which = PRIO_PGRP;
            continue;
        }
        if (strcmp(argv[i], "-u") == 0 || strcmp(argv[i], "--user") == 0) {
            which = PRIO_USER;
            continue;
        }

        id_t target_id = 0;
        if (which == PRIO_USER) {
            struct passwd *pw = getpwnam(argv[i]);
            if (pw) target_id = (id_t)pw->pw_uid;
            else target_id = (id_t)atoi(argv[i]);
        } else {
            target_id = (id_t)atoi(argv[i]);
        }

        errno = 0;
        int old_prio = getpriority(which, target_id);
        if (old_prio == -1 && errno != 0) {
            perror(argv[i]);
            ret = 1;
            continue;
        }

        if (setpriority(which, target_id, priority) < 0) {
            perror(argv[i]);
            ret = 1;
        } else {
            printf("%d (%s) old priority %d, new priority %d\n",
                   (int)target_id, (which == PRIO_PROCESS ? "process ID" : (which == PRIO_PGRP ? "process group" : "user")),
                   old_prio, priority);
        }
    }

    return ret;
}
