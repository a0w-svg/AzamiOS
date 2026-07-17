/**
 * awk.c — Field pattern scanning utility for AzamiOS
 * Supports '{print $N, $M, ...}' on whitespace-delimited columns.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdbool.h>

static void process_line(char *line, const char *action) {
    char *fields[32];
    int nf = 0;
    char *token = strtok(line, " \t");
    while (token && nf < 32) {
        fields[nf++] = token;
        token = strtok(NULL, " \t");
    }

    if (strstr(action, "print") != NULL) {
        const char *p = strchr(action, '$');
        if (!p) {
            /* Print whole line or all fields */
            for (int i = 0; i < nf; i++) printf("%s ", fields[i]);
            printf("\n");
        } else {
            bool first = true;
            while (p) {
                int col = atoi(p + 1);
                if (!first) printf(" ");
                if (col == 0) {
                    for (int i = 0; i < nf; i++) printf("%s ", fields[i]);
                } else if (col <= nf) {
                    printf("%s", fields[col - 1]);
                }
                first = false;
                p = strchr(p + 1, '$');
            }
            printf("\n");
        }
    }
}

static void process_fd(int fd, const char *action) {
    char buf[1024];
    int n, lpos = 0;
    char line[512];

    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < n; i++) {
            if (buf[i] == '\n' || lpos >= 511) {
                line[lpos] = '\0';
                process_line(line, action);
                lpos = 0;
            } else if (buf[i] != '\r') {
                line[lpos++] = buf[i];
            }
        }
    }
    if (lpos > 0) {
        line[lpos] = '\0';
        process_line(line, action);
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: awk '{print $1, $2}' [file]\n");
        return 1;
    }
    const char *action = argv[1];
    if (argc == 2) {
        process_fd(0, action);
    } else {
        int fd = open(argv[2], O_RDONLY);
        if (fd < 0) {
            printf("awk: cannot open %s\n", argv[2]);
            return 1;
        }
        process_fd(fd, action);
        close(fd);
    }
    return 0;
}
