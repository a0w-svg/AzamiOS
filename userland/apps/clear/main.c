/* ============================================================================
 * AzamiOS Userspace — Clear Screen (clear.elf)
 * File: userland/apps/clear/main.c
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/string.h"
#include "../../libc/include/unistd.h"
#include "../../libc/include/getopt.h"

static void print_help(void)
{
    printf("Usage: clear [options]\n\n"
           "Options:\n"
           "  -x          do not clear scrollback buffer\n"
           "  -T TERM     use this terminal type instead of $TERM\n"
           "  -V          print curses-version\n"
           "  -h, --help  display this help and exit\n");
}

static void print_version(void)
{
    printf("clear (AzamiOS coreutils) 7.0.0\n");
}

int main(int argc, char **argv)
{
    int no_scrollback = 0;
    const char *term = NULL;

    static struct option long_options[] = {
        {"help",    no_argument, 0, 'h'},
        {"version", no_argument, 0, 'V'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "xT:Vh", long_options, NULL)) != -1) {
        switch (opt) {
        case 'x':
            no_scrollback = 1;
            break;
        case 'T':
            term = optarg;
            (void)term;
            break;
        case 'V':
            print_version();
            return 0;
        case 'h':
            print_help();
            return 0;
        default:
            fprintf(stderr, "Try 'clear --help' for more information.\n");
            return 1;
        }
    }

    if (no_scrollback) {
        /* Standard clear without purging scrollback */
        printf("\033[H\033[2J");
    } else {
        /* Clear screen and erase scrollback buffer (ED 3 + ED 2 + CUP home) */
        printf("\033[3J\033[H\033[2J");
    }

    fflush(stdout);
    return 0;
}
