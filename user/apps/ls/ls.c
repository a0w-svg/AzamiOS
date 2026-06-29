#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <dirent.h>

void _start(void) {
    char path[128] = "/";
    int arg_fd = open("cmd_args", O_RDONLY);
    if (arg_fd >= 0) {
        char buf[128];
        int n = read(arg_fd, buf, 127);
        close(arg_fd);
        if (n > 0) {
            buf[n] = '\0';
            while (n > 0 && (buf[n - 1] == ' ' || buf[n - 1] == '\n' || buf[n - 1] == '\r')) {
                buf[--n] = '\0';
            }
            char *start = buf;
            while (*start == ' ') start++;
            if (*start != '\0') {
                strncpy(path, start, 127);
                path[127] = '\0';
            }
        }
    }

    DIR *dir = opendir(path);
    char out_buf[1024];
    out_buf[0] = '\0';
    int offset = 0;

    if (!dir) {
        strcpy(out_buf, "ls: cannot access '");
        strcat(out_buf, path);
        strcat(out_buf, "': No such file or directory\n");
    } else {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            int len = strlen(ent->d_name);
            if (offset + len + 2 < (int)sizeof(out_buf)) {
                strcpy(out_buf + offset, ent->d_name);
                offset += len;
                out_buf[offset++] = ' ';
                out_buf[offset] = '\0';
            }
        }
        closedir(dir);
        if (offset > 0) {
            out_buf[offset - 1] = '\n';
        } else {
            out_buf[0] = '\n';
            out_buf[1] = '\0';
        }
    }

    int fd = open("cmd_out", O_WRONLY | O_CREAT, 0);
    if (fd >= 0) {
        write(fd, out_buf, strlen(out_buf));
        close(fd);
    }
    exit(0);
}
