#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>

void _start(void) {
    const char *out = "\033WIN_NOTEPAD\nLaunching Notepad Editor...\n";
    int fd = open("cmd_out", O_WRONLY | O_CREAT, 0);
    if (fd >= 0) {
        write(fd, out, strlen(out));
        close(fd);
    }
    exit(0);
}
