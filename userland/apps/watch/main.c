/* ============================================================================
 * AzamiOS Userspace — Periodic Command Runner (watch.elf)
 * File: userland/apps/watch/main.c
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/string.h"
#include "../../libc/include/stdbool.h"
#include "../../libc/include/unistd.h"
#include "../../libc/include/sys/wait.h"
#include "../../libc/include/time.h"

static void print_usage(const char *prog)
{
    printf("Usage: %s [options] command\n\n"
           "Options:\n"
           "  -n, --interval <secs>  seconds to wait between updates (default: 2.0)\n"
           "  -t, --no-title         turn off header\n"
           "  -h, --help             display this help and exit\n"
           "  -v, --version          output version information and exit\n",
           prog);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    int interval = 2;
    bool no_title = false;
    int cmd_start = -1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "-V") == 0) {
            printf("watch from procps-ng (AzamiOS compatibility) 7.0\n");
            return 0;
        }
        if (strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--no-title") == 0) {
            no_title = true;
            continue;
        }
        if (strcmp(argv[i], "-n") == 0 || strcmp(argv[i], "--interval") == 0) {
            if (i + 1 < argc) {
                interval = atoi(argv[++i]);
                if (interval <= 0) interval = 1;
                continue;
            } else {
                fprintf(stderr, "watch: option '%s' requires an argument\n", argv[i]);
                return 1;
            }
        }
        if (argv[i][0] == '-' && argv[i][1] != '\0') {
            bool matched_opt = false;
            for (int j = 1; argv[i][j]; j++) {
                if (argv[i][j] == 't') {
                    no_title = true;
                    matched_opt = true;
                } else if (argv[i][j] == 'h') {
                    print_usage(argv[0]);
                    return 0;
                } else if (argv[i][j] == 'v' || argv[i][j] == 'V') {
                    printf("watch from procps-ng (AzamiOS compatibility) 7.0\n");
                    return 0;
                } else if (argv[i][j] == 'n') {
                    if (argv[i][j + 1] != '\0') {
                        interval = atoi(&argv[i][j + 1]);
                        if (interval <= 0) interval = 1;
                        matched_opt = true;
                        break;
                    } else if (i + 1 < argc) {
                        interval = atoi(argv[++i]);
                        if (interval <= 0) interval = 1;
                        matched_opt = true;
                        break;
                    }
                }
            }
            if (matched_opt) continue;
        }

        /* First non-option argument begins the command */
        cmd_start = i;
        break;
    }

    if (cmd_start < 0 || cmd_start >= argc) {
        fprintf(stderr, "watch: missing command\n"
                        "Try 'watch --help' for more information.\n");
        return 1;
    }

    char cmd_desc[256] = "";
    for (int i = cmd_start; i < argc; i++) {
        if (i > cmd_start) strcat(cmd_desc, " ");
        strncat(cmd_desc, argv[i], sizeof(cmd_desc) - strlen(cmd_desc) - 1);
    }

    while (1) {
        if (!no_title) {
            time_t now = time(NULL);
            struct tm tm_info;
            localtime_r(&now, &tm_info);
            char time_str[64] = "";
            snprintf(time_str, sizeof(time_str), "%02d:%02d:%02d %d",
                     tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec, tm_info.tm_year + 1900);

            printf("\033[H\033[2J"); /* Clear terminal screen */
            printf("Every %d.0s: %-30s %25s\n\n", interval, cmd_desc, time_str);
        } else {
            printf("\033[H\033[2J");
        }

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

            snprintf(bin_path, sizeof(bin_path), "/sbin/%s.elf", child_argv[0]);
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
