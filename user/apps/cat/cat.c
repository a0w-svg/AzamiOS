#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>

void _start(void) {
    char fname[64] = {0};
    int arg_fd = open("cmd_args", 0);
    if (arg_fd >= 0) {
        int n = read(arg_fd, fname, sizeof(fname) - 1);
        if (n > 0) fname[n] = '\0';
        close(arg_fd);
    }
    
    char *p = fname;
    while (*p == ' ') p++;

    int out_fd = open("cmd_out", O_WRONLY | O_CREAT, 0);
    if (out_fd < 0) exit(1);

    if (*p == '\0') {
        const char *msg = "cat: missing filename argument\n";
        write(out_fd, msg, strlen(msg));
        close(out_fd);
        exit(0);
    }

    int in_fd = open(p, 0);
    if (in_fd >= 0) {
        char buf[1024];
        int n = read(in_fd, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            write(out_fd, buf, n);
            if (buf[n - 1] != '\n') write(out_fd, "\n", 1);
        } else {
            const char *msg = "(empty file)\n";
            write(out_fd, msg, strlen(msg));
        }
        close(in_fd);
    } else {
        char msg[128];
        snprintf(msg, sizeof(msg), "cat: file not found: %s\n", p);
        write(out_fd, msg, strlen(msg));
    }
    close(out_fd);
    exit(0);
}
