#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <gui.h>

void _start(void) {
    sys_reboot();
    exit(0);
}
