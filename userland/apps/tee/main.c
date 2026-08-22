/* ============================================================================
 * AzamiOS Userspace — tee (Duplicate standard input)
 * File: userland/apps/tee/main.c
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>

int main(int argc, char **argv)
{
    int append = 0;
    int opt_idx = 1;

    while (opt_idx < argc && argv[opt_idx][0] == '-') {
        if (strcmp(argv[opt_idx], "-a") == 0) append = 1;
        else if (strcmp(argv[opt_idx], "--") == 0) { opt_idx++; break; }
        else break;
        opt_idx++;
    }

    int num_files = argc - opt_idx;
    int *fds = NULL;
    if (num_files > 0) {
        fds = (int *)malloc(sizeof(int) * num_files);
        if (!fds) return 1;
        for (int i = 0; i < num_files; i++) {
            int flags = O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC);
            fds[i] = open(argv[opt_idx + i], flags, 0644);
            if (fds[i] < 0) {
                perror(argv[opt_idx + i]);
            }
        }
    }

    char buf[4096];
    ssize_t n;
    while ((n = read(STDIN_FILENO, buf, sizeof(buf))) > 0) {
        write(STDOUT_FILENO, buf, n);
        for (int i = 0; i < num_files; i++) {
            if (fds && fds[i] >= 0) {
                write(fds[i], buf, n);
            }
        }
    }

    for (int i = 0; i < num_files; i++) {
        if (fds && fds[i] >= 0) close(fds[i]);
    }
    if (fds) free(fds);

    return 0;
}
