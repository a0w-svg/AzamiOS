/* ============================================================================
 * AzamiOS Userspace — cowsay (cowsay.elf)
 * File: userland/apps/cowsay/main.c
 *
 * A small, self-contained app with no reason to exist except to give the
 * package manager (pkg.elf) something real to install: `pkg install
 * cowsay` fetches this exact binary from the sample repository and drops
 * it at /bin/cowsay.elf, same as any other package.
 * ============================================================================ */

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    char msg[512] = "Moo?";
    if (argc > 1) {
        size_t off = 0;
        for (int i = 1; i < argc && off < sizeof(msg) - 1; i++) {
            int n = snprintf(msg + off, sizeof(msg) - off, "%s%s", i > 1 ? " " : "", argv[i]);
            if (n < 0) break;
            off += (size_t)n;
        }
    }

    size_t len = strlen(msg);
    printf(" ");
    for (size_t i = 0; i < len + 2; i++) printf("_");
    printf("\n< %s >\n", msg);
    printf(" ");
    for (size_t i = 0; i < len + 2; i++) printf("-");
    printf("\n");
    printf("        \\   ^__^\n");
    printf("         \\  (oo)\\_______\n");
    printf("            (__)\\       )\\/\\\n");
    printf("                ||----w |\n");
    printf("                ||     ||\n");
    return 0;
}
