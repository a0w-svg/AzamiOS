/* ============================================================================
 * AzamiOS Userspace — Linux pgrep / pkill Utility (main.c)
 * File: userland/apps/pgrep/main.c
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <signal.h>
#include <ctype.h>
#include <getopt.h>

static void print_help(int is_pkill)
{
    if (is_pkill) {
        printf("Usage: pkill [options] <pattern>\n"
               "Signal processes based on name.\n\n"
               "  -SIGNAL, -s SIGNAL  signal to send (default: 15 / TERM)\n"
               "  -f, --full          use full process name to match\n"
               "  -x, --exact         match exactly with the command name\n"
               "  -v, --inverse       negate the matching\n"
               "  -c, --count         count of matching processes\n"
               "  -h, --help          display this help and exit\n");
    } else {
        printf("Usage: pgrep [options] <pattern>\n"
               "List process IDs matching pattern.\n\n"
               "  -l, --list-name     list PID and process name\n"
               "  -f, --full          use full process name to match\n"
               "  -x, --exact         match exactly with the command name\n"
               "  -v, --inverse       negate the matching\n"
               "  -c, --count         count of matching processes\n"
               "  -d, --delimiter STR specify output delimiter\n"
               "  -h, --help          display this help and exit\n");
    }
}

static int parse_signal(const char *str)
{
    if (strcasecmp(str, "HUP") == 0 || strcmp(str, "1") == 0) return SIGHUP;
    if (strcasecmp(str, "INT") == 0 || strcmp(str, "2") == 0) return SIGINT;
    if (strcasecmp(str, "QUIT") == 0 || strcmp(str, "3") == 0) return SIGQUIT;
    if (strcasecmp(str, "KILL") == 0 || strcmp(str, "9") == 0) return SIGKILL;
    if (strcasecmp(str, "USR1") == 0 || strcmp(str, "10") == 0) return SIGUSR1;
    if (strcasecmp(str, "USR2") == 0 || strcmp(str, "12") == 0) return SIGUSR2;
    if (strcasecmp(str, "TERM") == 0 || strcmp(str, "15") == 0) return SIGTERM;
    int sig = atoi(str);
    return (sig > 0 && sig <= 64) ? sig : SIGTERM;
}

static int get_proc_cmdline(pid_t pid, char *out_name, size_t max_len, int full)
{
    char path[256];
    snprintf(path, sizeof(path), "/proc/%d/cmdline", (int)pid);
    FILE *f = fopen(path, "r");
    if (!f) {
        snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
        f = fopen(path, "r");
        if (!f) return -1;
        char line[256];
        if (fgets(line, sizeof(line), f)) {
            char *open_p = strchr(line, '(');
            char *close_p = strrchr(line, ')');
            if (open_p && close_p && close_p > open_p) {
                *close_p = '\0';
                strncpy(out_name, open_p + 1, max_len - 1);
                out_name[max_len - 1] = '\0';
                fclose(f);
                return 0;
            }
        }
        fclose(f);
        return -1;
    }

    if (fgets(out_name, max_len, f)) {
        if (!full) {
            char *base = strrchr(out_name, '/');
            if (base) memmove(out_name, base + 1, strlen(base + 1) + 1);
            char *dot = strstr(out_name, ".elf");
            if (dot) *dot = '\0';
        }
    } else {
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}

int main(int argc, char *argv[])
{
    int is_pkill = (strstr(argv[0], "pkill") != NULL);
    int sig = SIGTERM;
    int opt_full = 0;
    int opt_exact = 0;
    int opt_inverse = 0;
    int opt_count = 0;
    int opt_list = 0;
    const char *opt_delim = "\n";

    static struct option long_options[] = {
        {"full",        no_argument,       0, 'f'},
        {"exact",       no_argument,       0, 'x'},
        {"inverse",     no_argument,       0, 'v'},
        {"count",       no_argument,       0, 'c'},
        {"list-name",   no_argument,       0, 'l'},
        {"signal",      required_argument, 0, 's'},
        {"delimiter",   required_argument, 0, 'd'},
        {"help",        no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "fxvcls:d:h", long_options, NULL)) != -1) {
        switch (opt) {
            case 'f': opt_full = 1; break;
            case 'x': opt_exact = 1; break;
            case 'v': opt_inverse = 1; break;
            case 'c': opt_count = 1; break;
            case 'l': opt_list = 1; break;
            case 's': sig = parse_signal(optarg); is_pkill = 1; break;
            case 'd': opt_delim = optarg; break;
            case 'h': print_help(is_pkill); return 0;
            default:
                return 2;
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "%s: no pattern specified\n", argv[0]);
        return 2;
    }

    const char *pattern = argv[optind];

    DIR *dir = opendir("/proc");
    if (!dir) {
        perror("/proc");
        return 2;
    }

    pid_t my_pid = getpid();
    int match_count = 0;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9') continue;
        pid_t pid = (pid_t)atoi(entry->d_name);
        if (pid == my_pid) continue;

        char proc_name[256] = {0};
        if (get_proc_cmdline(pid, proc_name, sizeof(proc_name), opt_full) == 0) {
            int is_match = 0;
            if (opt_exact) {
                is_match = (strcmp(proc_name, pattern) == 0);
            } else {
                is_match = (strstr(proc_name, pattern) != NULL);
            }

            if (opt_inverse) is_match = !is_match;

            if (is_match) {
                match_count++;
                if (is_pkill) {
                    kill(pid, sig);
                } else if (!opt_count) {
                    if (opt_list) {
                        printf("%d %s%s", (int)pid, proc_name, opt_delim);
                    } else {
                        printf("%d%s", (int)pid, opt_delim);
                    }
                }
            }
        }
    }

    closedir(dir);

    if (!is_pkill && opt_count) {
        printf("%d\n", match_count);
    }

    return (match_count > 0) ? 0 : 1;
}
