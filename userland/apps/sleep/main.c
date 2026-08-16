/* ============================================================================
 * AzamiOS Userspace — Sleep Utility (sleep.elf)
 * File: userland/apps/sleep/main.c
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/time.h"
#include "../../libc/include/unistd.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: sleep <seconds>\n");
        return 1;
    }

    int seconds = atoi(argv[1]);
    if (seconds <= 0) return 0;

    struct timespec ts;
    ts.tv_sec = seconds;
    ts.tv_nsec = 0;

    nanosleep(&ts, NULL);
    return 0;
}
