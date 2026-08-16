/* ============================================================================
 * AzamiOS Userspace — Periodic Command Runner (watch.elf)
 * File: userland/apps/watch/main.c
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/string.h"
#include "../../libc/include/unistd.h"
#include "../../libc/include/sys/wait.h"
#include "../../libc/include/time.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: watch [-n sec] <command> [args...]\n");
        return 1;
    }

    int interval = 2;
    int cmd_start = 1;

    if (argc > 2 && strcmp(argv[1], "-n") == 0) {
        interval = atoi(argv[2]);
        if (interval <= 0) interval = 1;
        cmd_start = 3;
    }

    if (cmd_start >= argc) {
        fprintf(stderr, "watch: missing command\n");
        return 1;
    }

    char cmd_desc[256] = "";
    for (int i = cmd_start; i < argc; i++) {
        if (i > cmd_start) strcat(cmd_desc, " ");
        strncat(cmd_desc, argv[i], sizeof(cmd_desc) - strlen(cmd_desc) - 1);
    }

    while (1) {
        time_t now = time(NULL);
        struct tm *tm_info = localtime(&now);
        char time_str[64] = "";
        if (tm_info) {
            snprintf(time_str, sizeof(time_str), "%02d:%02d:%02d %d",
                     tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec, tm_info->tm_year + 1900);
        }

        printf("\033[H\033[2J"); /* Clear terminal screen */
        printf("Every %d.0s: %-30s %25s\n\n", interval, cmd_desc, time_str);

        pid_t pid = fork();
        if (pid == 0) {
            /* Child process */
            char *child_argv[32];
            int c = 0;
            for (int i = cmd_start; i < argc && c < 31; i++) {
                child_argv[c++] = argv[i];
            }
            child_argv[c] = NULL;

            /* Try direct path, /bin/, or / */
            execve(child_argv[0], child_argv, NULL);

            char bin_path[128];
            snprintf(bin_path, sizeof(bin_path), "/bin/%s.elf", child_argv[0]);
            execve(bin_path, child_argv, NULL);

            snprintf(bin_path, sizeof(bin_path), "/%s.elf", child_argv[0]);
            execve(bin_path, child_argv, NULL);

            fprintf(stderr, "watch: failed to execute '%s'\n", child_argv[0]);
            exit(127);
        } else if (pid > 0) {
            int status = 0;
            waitpid(pid, &status, 0);
        }

        sleep(interval);
    }

    return 0;
}
