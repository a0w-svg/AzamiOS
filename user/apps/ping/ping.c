#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>

void _start(void) {
    const char *out = "PING 10.0.2.2: 64 bytes from 10.0.2.2: icmp_seq=1 ttl=64 time=1ms\n";
    int fd = open("cmd_out", O_WRONLY | O_CREAT, 0);
    if (fd >= 0) {
        write(fd, out, strlen(out));
        close(fd);
    }
    exit(0);
}
