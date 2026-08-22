/* ============================================================================
 * AzamiOS Userspace — Build and Execute Commands from Stdin (xargs.elf)
 * File: userland/apps/xargs/main.c
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/string.h"
#include "../../libc/include/unistd.h"
#include "../../libc/include/sys/wait.h"

#define MAX_ARGS 128
#define LINE_BUF_SIZE 1024

int main(int argc, char **argv)
{
    char *cmd_args[MAX_ARGS];
    int base_argc = 0;

    if (argc > 1) {
        for (int i = 1; i < argc && base_argc < MAX_ARGS - 16; i++) {
            cmd_args[base_argc++] = argv[i];
        }
    } else {
        cmd_args[base_argc++] = "echo";
    }

    char line[LINE_BUF_SIZE];
    int l_idx = 0;
    char c;
    char *extra_args[MAX_ARGS];
    int extra_count = 0;

    while (read(0, &c, 1) > 0) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (l_idx > 0) {
                line[l_idx] = '\0';
                extra_args[extra_count++] = strdup(line);
                l_idx = 0;
                if (extra_count >= MAX_ARGS - base_argc - 2) {
                    /* Flush and execute batch */
                    for (int i = 0; i < extra_count; i++) {
                        cmd_args[base_argc + i] = extra_args[i];
                    }
                    cmd_args[base_argc + extra_count] = NULL;

                    int pid = fork();
                    if (pid == 0) {
                        execvp(cmd_args[0], cmd_args);
                        printf("xargs: %s: command not found\n", cmd_args[0]);
                        exit(127);
                    }
                    int status;
                    waitpid(pid, &status, 0);

                    for (int i = 0; i < extra_count; i++) free(extra_args[i]);
                    extra_count = 0;
                }
            }
        } else {
            if (l_idx < (int)sizeof(line) - 1) {
                line[l_idx++] = c;
            }
        }
    }

    if (l_idx > 0) {
        line[l_idx] = '\0';
        extra_args[extra_count++] = strdup(line);
    }

    if (extra_count > 0) {
        for (int i = 0; i < extra_count; i++) {
            cmd_args[base_argc + i] = extra_args[i];
        }
        cmd_args[base_argc + extra_count] = NULL;

        int pid = fork();
        if (pid == 0) {
            execvp(cmd_args[0], cmd_args);
            printf("xargs: %s: command not found\n", cmd_args[0]);
            exit(127);
        }
        int status;
        waitpid(pid, &status, 0);

        for (int i = 0; i < extra_count; i++) free(extra_args[i]);
    }

    return 0;
}
