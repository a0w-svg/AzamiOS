#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <gui.h>

void _start(void) {
    char mod_buf[1024] = {0};
    sys_lsmod(mod_buf, sizeof(mod_buf));
    int fd = open("cmd_out", O_WRONLY | O_CREAT, 0);
    if (fd >= 0) {
        write(fd, mod_buf, strlen(mod_buf));
        close(fd);
    }
    exit(0);
}
