/* ============================================================================
 * AzamiOS Userspace — Process Status Utility (ps.elf)
 * File: userland/apps/ps/main.c
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/string.h"
#include "../../libc/include/stdbool.h"
#include "../../libc/include/fcntl.h"
#include "../../libc/include/unistd.h"
#include "../../libc/include/dirent.h"
#include "../../libc/include/getopt.h"

typedef struct {
    int pid;
    int ppid;
    char state[16];
    char name[64];
    unsigned long vmsize_kb;
    char cmdline[128];
} proc_info_t;

static int read_proc_info(int pid, proc_info_t *info)
{
    char path[128];
    snprintf(path, sizeof(path), "/proc/%d/status", pid);

    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return -1;

    char buf[1024];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';

    info->pid = pid;
    info->ppid = 0;
    strcpy(info->state, "R");
    strcpy(info->name, "unknown");
    info->vmsize_kb = 64;
    strcpy(info->cmdline, "");

    /* Parse status key: values */
    char *line = strtok(buf, "\n");
    while (line) {
        if (strncmp(line, "Name:", 5) == 0) {
            char *val = line + 5;
            while (*val == ' ' || *val == '\t') val++;
            strncpy(info->name, val, sizeof(info->name) - 1);
        } else if (strncmp(line, "State:", 6) == 0) {
            char *val = line + 6;
            while (*val == ' ' || *val == '\t') val++;
            strncpy(info->state, val, 1);
            info->state[1] = '\0';
        } else if (strncmp(line, "PPid:", 5) == 0) {
            info->ppid = atoi(line + 5);
        } else if (strncmp(line, "VmSize:", 7) == 0) {
            info->vmsize_kb = (unsigned long)atoi(line + 7);
        }
        line = strtok(NULL, "\n");
    }

    /* Try to read cmdline */
    snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
    fd = open(path, O_RDONLY, 0);
    if (fd >= 0) {
        n = read(fd, info->cmdline, sizeof(info->cmdline) - 1);
        if (n > 0) {
            info->cmdline[n] = '\0';
            /* Strip trailing newline */
            if (info->cmdline[n - 1] == '\n') info->cmdline[n - 1] = '\0';
        }
        close(fd);
    }

    if (!info->cmdline[0]) {
        strncpy(info->cmdline, info->name, sizeof(info->cmdline) - 1);
    }

    return 0;
}

int main(int argc, char **argv)
{
    bool full = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "-ef") == 0 || strcmp(argv[i], "aux") == 0) {
            full = true;
        }
    }

    DIR *dir = opendir("/proc");
    if (!dir) {
        fprintf(stderr, "ps: cannot open /proc filesystem\n");
        return 1;
    }

    if (full) {
        printf("%-6s %-6s %-6s %-6s %8s %-20s\n", "UID", "PID", "PPID", "STAT", "VSZ", "CMD");
    } else {
        printf("%6s %6s %-6s %8s %-20s\n", "PID", "PPID", "STAT", "VSZ", "COMMAND");
    }

    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        /* Check if numeric PID directory */
        int pid = atoi(de->d_name);
        if (pid <= 0) continue;

        proc_info_t info;
        if (read_proc_info(pid, &info) == 0) {
            if (full) {
                printf("%-6s %6d %6d %-6s %7luK %-20s\n",
                       "root", info.pid, info.ppid, info.state, info.vmsize_kb, info.cmdline);
            } else {
                printf("%6d %6d %-6s %7luK %-20s\n",
                       info.pid, info.ppid, info.state, info.vmsize_kb, info.name);
            }
        }
    }

    closedir(dir);
    return 0;
}
