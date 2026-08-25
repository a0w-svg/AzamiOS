/* ============================================================================
 * AzamiOS Userspace — POSIX sync Utility (main.c)
 * File: userland/apps/sync/main.c
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <getopt.h>

static void print_help(void)
{
    printf("Usage: sync [OPTION] [FILE]...\n"
           "Synchronize cached writes to persistent storage\n\n"
           "  -d, --data            sync only file data, no unneeded metadata\n"
           "  -f, --file-system     sync the file systems that contain the files\n"
           "      --help            display this help and exit\n");
}

int main(int argc, char *argv[])
{
    int opt;
    int opt_data = 0;
    int opt_fs = 0;

    static struct option long_options[] = {
        {"data",        no_argument, 0, 'd'},
        {"file-system", no_argument, 0, 'f'},
        {"help",        no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    while ((opt = getopt_long(argc, argv, "dfh", long_options, NULL)) != -1) {
        switch (opt) {
            case 'd': opt_data = 1; break;
            case 'f': opt_fs = 1; break;
            case 'h': print_help(); return 0;
            default:
                fprintf(stderr, "Try 'sync --help' for more information.\n");
                return 1;
        }
    }

    if (optind >= argc) {
        sync();
        return 0;
    }

    int ret = 0;
    for (int i = optind; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            perror(argv[i]);
            ret = 1;
            continue;
        }
        if (opt_data) {
            if (fdatasync(fd) < 0) {
                perror("fdatasync");
                ret = 1;
            }
        } else if (opt_fs) {
            if (syncfs(fd) < 0) {
                perror("syncfs");
                ret = 1;
            }
        } else {
            if (fsync(fd) < 0) {
                perror("fsync");
                ret = 1;
            }
        }
        close(fd);
    }

    sync();
    return ret;
}
