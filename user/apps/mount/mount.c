/**
 * mount.c  –  AzamiOS Mount / Unmount Command Line Utility
 */
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/mount.h>

void _start(void) {
    char args[128] = {0};
    int arg_fd = open("cmd_args", O_RDONLY);
    if (arg_fd >= 0) {
        int n = read(arg_fd, args, sizeof(args) - 1);
        if (n > 0) args[n] = '\0';
        close(arg_fd);
    }

    int out_fd = open("cmd_out", O_WRONLY | O_CREAT, 0);
    if (out_fd < 0) exit(1);

    char *p = args;
    while (*p == ' ' || *p == '\t') p++;

    if (*p == '\0') {
        const char *msg = "Usage: mount <dev> <dir> [type]   OR   mount -u <dir>\n";
        write(out_fd, msg, strlen(msg));
        close(out_fd);
        exit(1);
    }

    if (strncmp(p, "-u ", 3) == 0) {
        p += 3;
        while (*p == ' ') p++;
        /* Strip trailing whitespace */
        int len = strlen(p);
        while (len > 0 && (p[len - 1] == ' ' || p[len - 1] == '\n' || p[len - 1] == '\r')) {
            p[--len] = '\0';
        }
        if (unmount(p) == 0) {
            char buf[128];
            snprintf(buf, sizeof(buf), "Unmounted [%s] successfully.\n", p);
            write(out_fd, buf, strlen(buf));
            close(out_fd);
            exit(0);
        } else {
            char buf[128];
            snprintf(buf, sizeof(buf), "Error: failed to unmount [%s].\n", p);
            write(out_fd, buf, strlen(buf));
            close(out_fd);
            exit(1);
        }
    }

    /* Parse <dev> <dir> [type] */
    char dev[64] = {0};
    char dir[64] = {0};
    char type[32] = "fat32";

    int i = 0;
    while (*p && *p != ' ' && i < 63) dev[i++] = *p++;
    dev[i] = '\0';

    while (*p == ' ') p++;
    i = 0;
    while (*p && *p != ' ' && *p != '\n' && *p != '\r' && i < 63) dir[i++] = *p++;
    dir[i] = '\0';

    while (*p == ' ') p++;
    if (*p && *p != '\n' && *p != '\r') {
        i = 0;
        while (*p && *p != ' ' && *p != '\n' && *p != '\r' && i < 31) type[i++] = *p++;
        type[i] = '\0';
    }

    if (dev[0] == '\0' || dir[0] == '\0') {
        const char *msg = "Usage: mount <dev> <dir> [type]\n";
        write(out_fd, msg, strlen(msg));
        close(out_fd);
        exit(1);
    }

    if (mount(dev, dir, type) == 0) {
        char buf[128];
        snprintf(buf, sizeof(buf), "Mounted [%s] on [%s] type [%s] successfully.\n", dev, dir, type);
        write(out_fd, buf, strlen(buf));
        close(out_fd);
        exit(0);
    } else {
        char buf[128];
        snprintf(buf, sizeof(buf), "Error: failed to mount [%s] on [%s].\n", dev, dir);
        write(out_fd, buf, strlen(buf));
        close(out_fd);
        exit(1);
    }
}
