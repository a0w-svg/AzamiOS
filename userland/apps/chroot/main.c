/* ============================================================================
 * AzamiOS Userspace — POSIX chroot Utility (main.c)
 * File: userland/apps/chroot/main.c
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void print_help(void)
{
    printf("Usage: chroot NEWROOT [COMMAND [ARG]...]\n"
           "Run COMMAND with root directory set to NEWROOT.\n\n"
           "If no command is given, run '${SHELL} -i' (default: '/bin/sh -i').\n\n"
           "      --help     display this help and exit\n");
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "chroot: missing operand\nTry 'chroot --help' for more information.\n");
        return 1;
    }

    if (strcmp(argv[1], "--help") == 0) {
        print_help();
        return 0;
    }

    const char *newroot = argv[1];
    if (chroot(newroot) < 0) {
        perror(newroot);
        return 1;
    }

    if (chdir("/") < 0) {
        perror("chdir");
        return 1;
    }

    if (argc > 2) {
        execvp(argv[2], &argv[2]);
        perror(argv[2]);
        return 127;
    }

    const char *shell = getenv("SHELL");
    if (!shell || !*shell) shell = "/bin/sh.elf";

    char *const sh_args[] = { (char *)shell, (char *)"-i", NULL };
    execvp(shell, sh_args);
    perror(shell);
    return 127;
}
