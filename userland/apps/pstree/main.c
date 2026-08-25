/* ============================================================================
 * AzamiOS Userland — Linux pstree Process Hierarchy Utility
 * File: userland/apps/pstree/main.c
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>

#define MAX_PROCS 128

typedef struct {
    int pid;
    int ppid;
    char name[64];
} proc_info_t;

static proc_info_t g_procs[MAX_PROCS];
static int g_num_procs = 0;

static void parse_proc_status(int pid)
{
    char path[128];
    snprintf(path, sizeof(path), "/proc/%d/status", pid);

    int fd = open(path, O_RDONLY);
    if (fd < 0) return;

    char buf[1024];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return;
    buf[n] = '\0';

    char name[64] = "process";
    int ppid = 0;

    char *line = strtok(buf, "\n");
    while (line) {
        if (strncmp(line, "Name:", 5) == 0) {
            char *p = line + 5;
            while (*p == ' ' || *p == '\t') p++;
            strncpy(name, p, sizeof(name) - 1);
            name[sizeof(name) - 1] = '\0';
        } else if (strncmp(line, "PPid:", 5) == 0) {
            char *p = line + 5;
            while (*p == ' ' || *p == '\t') p++;
            ppid = atoi(p);
        }
        line = strtok(NULL, "\n");
    }

    if (g_num_procs < MAX_PROCS) {
        g_procs[g_num_procs].pid = pid;
        g_procs[g_num_procs].ppid = ppid;
        strncpy(g_procs[g_num_procs].name, name, sizeof(g_procs[g_num_procs].name) - 1);
        g_procs[g_num_procs].name[sizeof(g_procs[g_num_procs].name) - 1] = '\0';
        g_num_procs++;
    }
}

static void scan_processes(void)
{
    DIR *dir = opendir("/proc");
    if (!dir) return;

    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (de->d_name[0] >= '0' && de->d_name[0] <= '9') {
            int pid = atoi(de->d_name);
            if (pid > 0) {
                parse_proc_status(pid);
            }
        }
    }
    closedir(dir);
}

static void print_tree(int pid, int depth, int is_last, const char *prefix, int show_pids)
{
    /* Find process index */
    int idx = -1;
    for (int i = 0; i < g_num_procs; i++) {
        if (g_procs[i].pid == pid) {
            idx = i;
            break;
        }
    }
    if (idx < 0) return;

    if (depth == 0) {
        if (show_pids) {
            printf("%s(%d)\n", g_procs[idx].name, g_procs[idx].pid);
        } else {
            printf("%s\n", g_procs[idx].name);
        }
    } else {
        printf("%s%s── ", prefix, is_last ? "└" : "├");
        if (show_pids) {
            printf("%s(%d)\n", g_procs[idx].name, g_procs[idx].pid);
        } else {
            printf("%s\n", g_procs[idx].name);
        }
    }

    /* Count children */
    int children[MAX_PROCS];
    int child_cnt = 0;
    for (int i = 0; i < g_num_procs; i++) {
        if (g_procs[i].ppid == pid && g_procs[i].pid != pid) {
            children[child_cnt++] = g_procs[i].pid;
        }
    }

    char next_prefix[256];
    if (depth == 0) {
        snprintf(next_prefix, sizeof(next_prefix), " ");
    } else {
        snprintf(next_prefix, sizeof(next_prefix), "%s%s   ", prefix, is_last ? " " : "│");
    }

    for (int i = 0; i < child_cnt; i++) {
        print_tree(children[i], depth + 1, (i == child_cnt - 1), next_prefix, show_pids);
    }
}

int main(int argc, char **argv)
{
    int show_pids = 0;
    int target_pid = 1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--show-pids") == 0) {
            show_pids = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("Usage: pstree [options] [pid]\n");
            printf("Options:\n");
            printf("  -p, --show-pids   Show PIDs in output\n");
            printf("  -h, --help        Display this help\n");
            return 0;
        } else if (argv[i][0] >= '0' && argv[i][0] <= '9') {
            target_pid = atoi(argv[i]);
        }
    }

    scan_processes();

    if (g_num_procs == 0) {
        printf("init(1)\n");
        return 0;
    }

    print_tree(target_pid, 0, 1, "", show_pids);
    return 0;
}
