#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <time.h>

void _start(void) {
    rtc_time_t t;
    rtc_get_time(&t);
    char buf[128];
    snprintf(buf, sizeof(buf), "Hardware RTC Time: %02d:%02d:%02d (%04d-%02d-%02d)\n",
             t.hour, t.minute, t.second, t.year, t.month, t.day);
    int fd = open("cmd_out", O_WRONLY | O_CREAT, 0);
    if (fd >= 0) {
        write(fd, buf, strlen(buf));
        close(fd);
    }
    exit(0);
}
