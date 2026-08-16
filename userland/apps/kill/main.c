/* ============================================================================
 * AzamiOS Userspace — Send Signal (kill.elf)
 * File: userland/apps/kill/main.c
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/string.h"
#include "../../libc/include/signal.h"

static int parse_signal(const char *s)
{
    if (s[0] >= '0' && s[0] <= '9') {
        return atoi(s);
    }
    if (strcasecmp(s, "HUP") == 0) return SIGHUP;
    if (strcasecmp(s, "INT") == 0) return SIGINT;
    if (strcasecmp(s, "QUIT") == 0) return SIGQUIT;
    if (strcasecmp(s, "ILL") == 0) return SIGILL;
    if (strcasecmp(s, "ABRT") == 0) return SIGABRT;
    if (strcasecmp(s, "FPE") == 0) return SIGFPE;
    if (strcasecmp(s, "KILL") == 0) return SIGKILL;
    if (strcasecmp(s, "SEGV") == 0) return SIGSEGV;
    if (strcasecmp(s, "PIPE") == 0) return SIGPIPE;
    if (strcasecmp(s, "ALRM") == 0) return SIGALRM;
    if (strcasecmp(s, "TERM") == 0) return SIGTERM;
    if (strcasecmp(s, "USR1") == 0) return SIGUSR1;
    if (strcasecmp(s, "USR2") == 0) return SIGUSR2;
    if (strcasecmp(s, "CHLD") == 0) return SIGCHLD;
    if (strcasecmp(s, "CONT") == 0) return SIGCONT;
    if (strcasecmp(s, "STOP") == 0) return SIGSTOP;
    return SIGTERM;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: kill [-s sig | -sig] pid ...\n");
        return 1;
    }

    int sig = SIGTERM;
    int pid_start = 1;

    if (argv[1][0] == '-') {
        if (strcmp(argv[1], "-s") == 0 && argc > 3) {
            sig = parse_signal(argv[2]);
            pid_start = 3;
        } else if (strcmp(argv[1], "-l") == 0) {
            printf(" 1) SIGHUP   2) SIGINT   3) SIGQUIT  4) SIGILL\n");
            printf(" 6) SIGABRT  8) SIGFPE   9) SIGKILL 11) SIGSEGV\n");
            printf("13) SIGPIPE 14) SIGALRM 15) SIGTERM 17) SIGCHLD\n");
            return 0;
        } else {
            sig = parse_signal(&argv[1][1]);
            pid_start = 2;
        }
    }

    int ret = 0;
    for (int i = pid_start; i < argc; i++) {
        int pid = atoi(argv[i]);
        if (pid <= 0) {
            printf("kill: invalid pid '%s'\n", argv[i]);
            ret = 1;
            continue;
        }
        if (kill(pid, sig) != 0) {
            printf("kill: (%d) - No such process or failed\n", pid);
            ret = 1;
        }
    }
    return ret;
}
