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
    snprintf(buf, sizeof(buf), "Hardware RTC Time: %02u:%02u:%02u (%04u-%02u-%02u)\n",
             (unsigned)t.hour, (unsigned)t.minute, (unsigned)t.second,
             (unsigned)t.year, (unsigned)t.month, (unsigned)t.day);
    int fd = open("cmd_out", O_WRONLY | O_CREAT, 0);
    if (fd >= 0) {
        write(fd, buf, strlen(buf));
        close(fd);
    }
    exit(0);
}
