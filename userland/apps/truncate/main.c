/* ============================================================================
 * AzamiOS Userspace — File Truncate / Extend Tool (truncate)
 * File: userland/apps/truncate/main.c
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

int main(int argc, char **argv)
{
    long size = -1;
    const char *path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            size = atol(argv[++i]);
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("Usage: truncate -s SIZE FILE...\n");
            printf("Shrink or extend the size of each FILE to the specified size.\n");
            return 0;
        } else if (argv[i][0] != '-') {
            path = argv[i];
        }
    }

    if (!path || size < 0) {
        fprintf(stderr, "truncate: missing argument or negative size\nTry 'truncate --help' for more information.\n");
        return 1;
    }

    /* Ensure file exists or create it */
    int fd = open(path, O_WRONLY | O_CREAT, 0644);
    if (fd < 0) {
        perror("open");
        return 1;
    }
    close(fd);

    if (truncate(path, (off_t)size) < 0) {
        perror("truncate");
        return 1;
    }

    return 0;
}
