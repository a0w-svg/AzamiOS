/* ============================================================================
 * AzamiOS Userspace — Linux killall Utility (main.c)
 * File: userland/apps/killall/main.c
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <signal.h>
#include <ctype.h>
#include <getopt.h>

static void print_help(void)
{
    printf("Usage: killall [OPTION]... NAME...\n"
           "Kill processes by name.\n\n"
           "  -e, --exact          require an exact match for very long names\n"
           "  -s, --signal SIGNAL  send this signal instead of SIGTERM\n"
           "  -q, --quiet          do not complain if no processes were killed\n"
           "  -v, --verbose        report if the signal was successfully sent\n"
           "  -l, --list           list all known signal names\n"
           "      --help           display this help and exit\n");
}

static void list_signals(void)
{
    printf(" 1) SIGHUP       2) SIGINT       3) SIGQUIT      4) SIGILL\n"
           " 5) SIGTRAP      6) SIGABRT      7) SIGBUS       8) SIGFPE\n"
           " 9) SIGKILL     10) SIGUSR1     11) SIGSEGV     12) SIGUSR2\n"
           "13) SIGPIPE     14) SIGALRM     15) SIGTERM     17) SIGCHLD\n"
           "18) SIGCONT     19) SIGSTOP     20) SIGTSTP     28) SIGWINCH\n");
}

static int parse_signal(const char *str)
{
    if (strcasecmp(str, "HUP") == 0 || strcmp(str, "1") == 0 || strcasecmp(str, "SIGHUP") == 0) return SIGHUP;
    if (strcasecmp(str, "INT") == 0 || strcmp(str, "2") == 0 || strcasecmp(str, "SIGINT") == 0) return SIGINT;
    if (strcasecmp(str, "QUIT") == 0 || strcmp(str, "3") == 0 || strcasecmp(str, "SIGQUIT") == 0) return SIGQUIT;
    if (strcasecmp(str, "KILL") == 0 || strcmp(str, "9") == 0 || strcasecmp(str, "SIGKILL") == 0) return SIGKILL;
    if (strcasecmp(str, "USR1") == 0 || strcmp(str, "10") == 0 || strcasecmp(str, "SIGUSR1") == 0) return SIGUSR1;
    if (strcasecmp(str, "USR2") == 0 || strcmp(str, "12") == 0 || strcasecmp(str, "SIGUSR2") == 0) return SIGUSR2;
    if (strcasecmp(str, "TERM") == 0 || strcmp(str, "15") == 0 || strcasecmp(str, "SIGTERM") == 0) return SIGTERM;
    if (strcasecmp(str, "STOP") == 0 || strcmp(str, "19") == 0 || strcasecmp(str, "SIGSTOP") == 0) return SIGSTOP;
    if (strcasecmp(str, "CONT") == 0 || strcmp(str, "18") == 0 || strcasecmp(str, "SIGCONT") == 0) return SIGCONT;
    int sig = atoi(str);
    return (sig > 0 && sig <= 64) ? sig : SIGTERM;
}

static int get_proc_name(pid_t pid, char *out_name, size_t max_len)
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
        char *p = strchr(out_name, '\0');
        if (p) *p = '\0';
        char *base = strrchr(out_name, '/');
        if (base) {
            memmove(out_name, base + 1, strlen(base + 1) + 1);
        }
        /* Strip .elf extension if present */
        char *dot = strstr(out_name, ".elf");
        if (dot) *dot = '\0';
    } else {
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}

int main(int argc, char *argv[])
{
    int sig = SIGTERM;
    int opt_quiet = 0;
    int opt_verbose = 0;
    int arg_start = 1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            print_help();
            return 0;
        }
        if (strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "--list") == 0) {
            list_signals();
            return 0;
        }
        if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--quiet") == 0) {
            opt_quiet = 1;
            arg_start = i + 1;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            opt_verbose = 1;
            arg_start = i + 1;
        } else if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--signal") == 0) {
            if (i + 1 < argc) {
                sig = parse_signal(argv[++i]);
                arg_start = i + 1;
            }
        } else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            sig = parse_signal(argv[i] + 1);
            arg_start = i + 1;
        } else {
            break;
        }
    }

    if (arg_start >= argc) {
        if (!opt_quiet) fprintf(stderr, "killall: no process name specified\n");
        return 1;
    }

    DIR *dir = opendir("/proc");
    if (!dir) {
        if (!opt_quiet) perror("/proc");
        return 1;
    }

    pid_t my_pid = getpid();
    int total_killed = 0;

    for (int idx = arg_start; idx < argc; idx++) {
        const char *target_name = argv[idx];
        int matched = 0;
        rewinddir(dir);

        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (entry->d_name[0] < '0' || entry->d_name[0] > '9') continue;
            pid_t pid = (pid_t)atoi(entry->d_name);
            if (pid <= 1 || pid == my_pid) continue;

            char proc_name[128] = {0};
            if (get_proc_name(pid, proc_name, sizeof(proc_name)) == 0) {
                if (strcmp(proc_name, target_name) == 0 ||
                    (strstr(proc_name, target_name) != NULL)) {
                    if (kill(pid, sig) == 0) {
                        if (opt_verbose) {
                            printf("Killed %s(%d) with signal %d\n", proc_name, (int)pid, sig);
                        }
                        matched++;
                        total_killed++;
                    }
                }
            }
        }

        if (matched == 0 && !opt_quiet) {
            fprintf(stderr, "%s: no process found\n", target_name);
        }
    }

    closedir(dir);
    return (total_killed > 0) ? 0 : 1;
}
