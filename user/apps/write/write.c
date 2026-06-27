#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>

void _start(void) {
    char args[512] = {0};
    int arg_fd = open("cmd_args", 0);
    if (arg_fd >= 0) {
        int n = read(arg_fd, args, sizeof(args) - 1);
        if (n > 0) args[n] = '\0';
        close(arg_fd);
    }

    char *p = args;
    while (*p == ' ') p++;

    int out_fd = open("cmd_out", O_WRONLY | O_CREAT, 0);
    if (out_fd < 0) exit(1);

    char fname[64];
    int i = 0;
    while (*p && *p != ' ' && i < 63) fname[i++] = *p++;
    fname[i] = '\0';
    while (*p == ' ') p++;

    if (fname[0] == '\0') {
        const char *msg = "write: usage: write <filename> <content>\n";
        write(out_fd, msg, strlen(msg));
        close(out_fd);
        exit(0);
    }

    int fd = open(fname, O_WRONLY | O_CREAT, 0);
    if (fd >= 0) {
        write(fd, p, strlen(p));
        close(fd);
        char msg[128];
        snprintf(msg, sizeof(msg), "Written live to file [%s] without reboot.\n", fname);
        write(out_fd, msg, strlen(msg));
    } else {
        const char *msg = "write: failed to open/create file.\n";
        write(out_fd, msg, strlen(msg));
    }
    close(out_fd);
    exit(0);
}
