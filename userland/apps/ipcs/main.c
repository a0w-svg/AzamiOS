/* ============================================================================
 * ipcs — report System V IPC facilities
 * File: userland/apps/ipcs/main.c
 *
 * Reads the same /proc/sysvipc files Linux's ipcs does, so the kernel is the
 * single source of truth and this stays a formatter.  With no options every
 * facility is listed; -m, -s and -q select one.
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#define LINE_MAX_LEN 256

/* Split a whitespace-separated line into at most @max fields. */
static int split(char *line, char **out, int max)
{
    int n = 0;
    char *p = line;
    while (*p && n < max) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '\n') break;
        out[n++] = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
        if (*p) *p++ = '\0';
    }
    return n;
}

/*
 * Render one /proc/sysvipc file.  @cols names the columns to print and
 * @idx gives their positions in the kernel's line, so each facility's
 * different layout is described rather than special-cased.
 */
static int dump(const char *path, const char *title, const char *header,
                const int *idx, int ncols)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "ipcs: %s: %s\n", path, strerror(errno));
        return -1;
    }

    static char buf[8192];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n < 0) return -1;
    buf[n] = '\0';

    printf("------ %s --------\n%s\n", title, header);

    char *line = buf;
    int lineno = 0;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        /* The kernel emits its own header line first; skip it. */
        if (lineno++ > 0 && *line) {
            char *f[20];
            int got = split(line, f, 20);
            for (int i = 0; i < ncols; i++) {
                printf("%-11s", idx[i] < got ? f[idx[i]] : "-");
            }
            printf("\n");
        }
        line = nl ? nl + 1 : NULL;
    }
    printf("\n");
    return 0;
}

/* Column positions within each /proc/sysvipc line. */
static const int shm_cols[] = { 0, 1, 8, 2, 3, 6 };   /* key id owner perms bytes nattch */
static const int sem_cols[] = { 0, 1, 5, 2, 3 };      /* key id owner perms nsems      */
static const int msg_cols[] = { 0, 1, 7, 2, 3, 4 };   /* key id owner perms bytes msgs */

static void usage(void)
{
    printf("Usage: ipcs [-m] [-q] [-s] [-a]\n"
           "  -m   shared memory segments\n"
           "  -q   message queues\n"
           "  -s   semaphore arrays\n"
           "  -a   all (the default)\n");
}

int main(int argc, char **argv)
{
    int want_shm = 0, want_sem = 0, want_msg = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-m") == 0)       want_shm = 1;
        else if (strcmp(argv[i], "-s") == 0)  want_sem = 1;
        else if (strcmp(argv[i], "-q") == 0)  want_msg = 1;
        else if (strcmp(argv[i], "-a") == 0)  want_shm = want_sem = want_msg = 1;
        else { usage(); return strcmp(argv[i], "-h") == 0 ? 0 : 1; }
    }
    if (!want_shm && !want_sem && !want_msg) want_shm = want_sem = want_msg = 1;

    if (want_msg) {
        dump("/proc/sysvipc/msg", "Message Queues",
             "key        msqid      owner      perms      used-bytes messages",
             msg_cols, 6);
    }
    if (want_shm) {
        dump("/proc/sysvipc/shm", "Shared Memory Segments",
             "key        shmid      owner      perms      bytes      nattch",
             shm_cols, 6);
    }
    if (want_sem) {
        dump("/proc/sysvipc/sem", "Semaphore Arrays",
             "key        semid      owner      perms      nsems",
             sem_cols, 5);
    }
    return 0;
}
