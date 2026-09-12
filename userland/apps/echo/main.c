/* ============================================================================
 * AzamiOS Userspace — Echo Utility (echo.elf)
 * File: userland/apps/echo/main.c
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/string.h"
#include "../../libc/include/stdbool.h"

static void print_help(void)
{
    printf("Usage: echo [SHORT-OPTION]... [STRING]...\n"
           "  or:  echo LONG-OPTION\n"
           "Echo the STRING(s) to standard output.\n\n"
           "  -n             do not output the trailing newline\n"
           "  -e             enable interpretation of backslash escapes\n"
           "  -E             disable interpretation of backslash escapes (default)\n"
           "      --help     display this help and exit\n"
           "      --version  output version information and exit\n");
}

static void print_version(void)
{
    printf("echo (AzamiOS coreutils) 7.0.0\n");
}

static void print_escaped(const char *s)
{
    for (size_t i = 0; s[i] != '\0'; i++) {
        if (s[i] == '\\' && s[i + 1] != '\0') {
            i++;
            switch (s[i]) {
            case 'a': putchar('\a'); break;
            case 'b': putchar('\b'); break;
            case 'c': return; /* suppress further output */
            case 'e': putchar('\033'); break;
            case 'f': putchar('\f'); break;
            case 'n': putchar('\n'); break;
            case 'r': putchar('\r'); break;
            case 't': putchar('\t'); break;
            case 'v': putchar('\v'); break;
            case '\\': putchar('\\'); break;
            case '0': {
                /* Octal sequence up to 3 digits */
                int val = 0;
                int count = 0;
                while (count < 3 && s[i + 1] >= '0' && s[i + 1] <= '7') {
                    val = (val << 3) | (s[++i] - '0');
                    count++;
                }
                putchar((char)val);
                break;
            }
            default:
                putchar('\\');
                putchar(s[i]);
                break;
            }
        } else {
            putchar(s[i]);
        }
    }
}

int main(int argc, char **argv)
{
    if (argc == 2) {
        if (strcmp(argv[1], "--help") == 0) {
            print_help();
            return 0;
        }
        if (strcmp(argv[1], "--version") == 0) {
            print_version();
            return 0;
        }
    }

    bool newline = true;
    bool interpret_escapes = false;
    int start = 1;

    /* Parse leading flags: -n, -e, -E (and combinations like -ne, -en) */
    while (start < argc && argv[start][0] == '-' && argv[start][1] != '\0') {
        const char *p = &argv[start][1];
        bool valid = true;
        for (const char *c = p; *c != '\0'; c++) {
            if (*c != 'n' && *c != 'e' && *c != 'E') {
                valid = false;
                break;
            }
        }
        if (!valid) break;

        for (const char *c = p; *c != '\0'; c++) {
            if (*c == 'n') newline = false;
            else if (*c == 'e') interpret_escapes = true;
            else if (*c == 'E') interpret_escapes = false;
        }
        start++;
    }

    for (int i = start; i < argc; i++) {
        if (interpret_escapes) {
            print_escaped(argv[i]);
        } else {
            fputs(argv[i], stdout);
        }
        if (i < argc - 1) {
            putchar(' ');
        }
    }

    if (newline) {
        putchar('\n');
    }

    fflush(stdout);
    return 0;
}
