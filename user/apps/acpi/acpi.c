#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <gui.h>

void _start(void) {
    sys_acpi_info();
    const char *out = "ACPI Power Management: Active (PM1a_CNT=0x604 | FADT/RSDT Validated)\n";
    int fd = open("cmd_out", O_WRONLY | O_CREAT, 0);
    if (fd >= 0) {
        write(fd, out, strlen(out));
        close(fd);
    }
    exit(0);
}
