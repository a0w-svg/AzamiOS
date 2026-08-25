/* ============================================================================
 * AzamiOS Userland — Linux lsof (List Open Files) Utility
 * File: userland/apps/lsof/main.c
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

static void get_proc_cmd(int pid, char *out_name, size_t max_len)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
    int fd = open(path, O_RDONLY);
    if (fd >= 0) {
        ssize_t n = read(fd, out_name, max_len - 1);
        close(fd);
        if (n > 0) {
            out_name[n] = '\0';
            char *nl = strchr(out_name, '\n');
            if (nl) *nl = '\0';
            return;
        }
    }
    snprintf(out_name, max_len, "proc-%d", pid);
}

static void list_proc_fds(int pid, const char *cmd)
{
    char fd_dir_path[64];
    snprintf(fd_dir_path, sizeof(fd_dir_path), "/proc/%d/fd", pid);

    DIR *d = opendir(fd_dir_path);
    if (!d) return;

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;

        char link_path[128];
        snprintf(link_path, sizeof(link_path), "%s/%s", fd_dir_path, de->d_name);

        char target[256];
        ssize_t len = readlink(link_path, target, sizeof(target) - 1);
        if (len <= 0) {
            strncpy(target, "unknown", sizeof(target) - 1);
            target[sizeof(target) - 1] = '\0';
        } else {
            target[len] = '\0';
        }

        const char *type = "REG";
        if (strstr(target, "/dev/tty") != NULL || strstr(target, "/dev/console") != NULL) type = "CHR";
        else if (strstr(target, "/dev/") != NULL) type = "CHR";
        else if (strstr(target, "pipe:") != NULL || strstr(target, "FIFO") != NULL) type = "FIFO";
        else if (strstr(target, "socket:") != NULL) type = "SOCK";
        else if (strstr(target, "anon_inode") != NULL) type = "anon";

        printf("%-10s %5d   root %4su   %-5s      0       0 %s\n",
               cmd, pid, de->d_name, type, target);
    }
    closedir(d);
}

int main(int argc, char **argv)
{
    int filter_pid = -1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            filter_pid = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("Usage: lsof [options]\n");
            printf("Options:\n");
            printf("  -p <pid>   List open files for specified PID\n");
            printf("  -h, --help Display this help\n");
            return 0;
        }
    }

    printf("%-10s %5s %6s %4s   %-5s %6s %7s %s\n",
           "COMMAND", "PID", "USER", "FD", "TYPE", "DEVICE", "SIZE/OFF", "NAME");

    if (filter_pid > 0) {
        char cmd[64];
        get_proc_cmd(filter_pid, cmd, sizeof(cmd));
        list_proc_fds(filter_pid, cmd);
        return 0;
    }

    DIR *d = opendir("/proc");
    if (!d) {
        fprintf(stderr, "lsof: cannot open /proc\n");
        return 1;
    }

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] >= '0' && de->d_name[0] <= '9') {
            int pid = atoi(de->d_name);
            if (pid > 0) {
                char cmd[64];
                get_proc_cmd(pid, cmd, sizeof(cmd));
                list_proc_fds(pid, cmd);
            }
        }
    }
    closedir(d);

    return 0;
}
