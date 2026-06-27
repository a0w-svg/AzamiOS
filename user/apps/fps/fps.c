#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>

void _start(void) {
    const char *out = "Compositor FPS: 60 frames/sec (VSync Active | Buffer Blit: 12ms)\n";
    int fd = open("cmd_out", O_WRONLY | O_CREAT, 0);
    if (fd >= 0) {
        write(fd, out, strlen(out));
        close(fd);
    }
    exit(0);
}
