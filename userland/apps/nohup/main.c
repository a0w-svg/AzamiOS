/* ============================================================================
 * AzamiOS Userspace — POSIX nohup Utility (main.c)
 * File: userland/apps/nohup/main.c
 * ============================================================================ */
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/wait.h>

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "usage: nohup utility [argument...]\n");
        return 127;
    }

    signal(SIGHUP, SIG_IGN);

    if (isatty(1)) {
        int fd = open("nohup.out", O_WRONLY | O_CREAT | O_APPEND, 0600);
        if (fd >= 0) {
            dup2(fd, 1);
            close(fd);
            fprintf(stderr, "nohup: appending output to 'nohup.out'\n");
        }
    }
    if (isatty(2)) {
        dup2(1, 2);
    }

    execvp(argv[1], &argv[1]);
    fprintf(stderr, "nohup: failed to execute '%s'\n", argv[1]);
    return 127;
}
