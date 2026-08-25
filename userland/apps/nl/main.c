/* ============================================================================
 * AzamiOS Userspace — POSIX nl (Line Numbering) Utility (main.c)
 * File: userland/apps/nl/main.c
 * ============================================================================ */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void process_file(FILE *fp, int *lineno)
{
    char buf[4096];
    while (fgets(buf, sizeof(buf), fp)) {
        if (buf[0] == '\n' || buf[0] == '\r') {
            printf("\n");
        } else {
            printf("%6d\t%s", (*lineno)++, buf);
        }
    }
}

int main(int argc, char *argv[])
{
    int lineno = 1;
    if (argc <= 1) {
        process_file(stdin, &lineno);
        return 0;
    }
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-") == 0) {
            process_file(stdin, &lineno);
        } else {
            FILE *fp = fopen(argv[i], "r");
            if (!fp) {
                fprintf(stderr, "nl: cannot open '%s'\n", argv[i]);
                continue;
            }
            process_file(fp, &lineno);
            fclose(fp);
        }
    }
    return 0;
}
