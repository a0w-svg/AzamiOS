/* ============================================================================
 * AzamiOS Userland — Linux lsblk (List Block Devices) Utility
 * File: userland/apps/lsblk/main.c
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>

typedef struct {
    char name[32];
    char maj_min[16];
    char size_str[16];
    int rm;
    int ro;
    char type[16];
    char mountpoint[64];
    int is_part;
    char parent[32];
} blk_info_t;

static void read_sys_attr(const char *dev, const char *attr, char *out, size_t max_len)
{
    char path[128];
    snprintf(path, sizeof(path), "/sys/class/block/%s/%s", dev, attr);
    int fd = open(path, O_RDONLY);
    if (fd >= 0) {
        ssize_t n = read(fd, out, max_len - 1);
        close(fd);
        if (n > 0) {
            out[n] = '\0';
            char *nl = strchr(out, '\n');
            if (nl) *nl = '\0';
            return;
        }
    }
    out[0] = '\0';
}

static void format_size(unsigned long long sectors, char *out, size_t max_len)
{
    unsigned long long bytes = sectors * 512ULL;
    if (bytes >= 1024ULL * 1024 * 1024) {
        snprintf(out, max_len, "%.1fG", (double)bytes / (1024.0 * 1024 * 1024));
    } else if (bytes >= 1024ULL * 1024) {
        snprintf(out, max_len, "%lluM", bytes / (1024ULL * 1024));
    } else if (bytes >= 1024ULL) {
        snprintf(out, max_len, "%lluK", bytes / 1024ULL);
    } else {
        snprintf(out, max_len, "%lluB", bytes);
    }
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    DIR *d = opendir("/sys/class/block");
    if (!d) {
        /* Fallback if /sys not mounted: print default devices */
        printf("%-10s %7s %2s %7s %2s %-6s %s\n",
               "NAME", "MAJ:MIN", "RM", "SIZE", "RO", "TYPE", "MOUNTPOINTS");
        printf("%-10s %7s %2d %7s %2d %-6s %s\n",
               "sda", "8:0", 0, "2.0G", 0, "disk", "");
        printf("├─%-8s %7s %2d %7s %2d %-6s %s\n",
               "sda1", "8:1", 0, "2.0G", 0, "part", "/hdd");
        printf("%-10s %7s %2d %7s %2d %-6s %s\n",
               "loop0", "7:0", 0, "200M", 0, "loop", "");
        printf("%-10s %7s %2d %7s %2d %-6s %s\n",
               "ram0", "1:0", 0, "200M", 0, "ram", "/");
        return 0;
    }

    printf("%-10s %7s %2s %7s %2s %-6s %s\n",
           "NAME", "MAJ:MIN", "RM", "SIZE", "RO", "TYPE", "MOUNTPOINTS");

    blk_info_t list[32];
    int count = 0;

    struct dirent *de;
    while ((de = readdir(d)) != NULL && count < 32) {
        if (de->d_name[0] == '.') continue;

        blk_info_t *bi = &list[count++];
        strncpy(bi->name, de->d_name, sizeof(bi->name) - 1);
        bi->name[sizeof(bi->name) - 1] = '\0';

        char buf[64];
        read_sys_attr(bi->name, "dev", bi->maj_min, sizeof(bi->maj_min));
        if (bi->maj_min[0] == '\0') strncpy(bi->maj_min, "0:0", sizeof(bi->maj_min) - 1);

        read_sys_attr(bi->name, "size", buf, sizeof(buf));
        unsigned long long sec = buf[0] ? strtoull(buf, NULL, 10) : 0;
        format_size(sec, bi->size_str, sizeof(bi->size_str));

        read_sys_attr(bi->name, "removable", buf, sizeof(buf));
        bi->rm = atoi(buf);
        bi->ro = 0;

        if (strstr(bi->name, "loop") != NULL) {
            strncpy(bi->type, "loop", sizeof(bi->type) - 1);
            bi->is_part = 0;
            bi->mountpoint[0] = '\0';
        } else if (strstr(bi->name, "ram") != NULL) {
            strncpy(bi->type, "ram", sizeof(bi->type) - 1);
            bi->is_part = 0;
            strncpy(bi->mountpoint, "/", sizeof(bi->mountpoint) - 1);
        } else if (bi->name[strlen(bi->name) - 1] >= '0' && bi->name[strlen(bi->name) - 1] <= '9') {
            strncpy(bi->type, "part", sizeof(bi->type) - 1);
            bi->is_part = 1;
            strncpy(bi->mountpoint, "/hdd", sizeof(bi->mountpoint) - 1);
        } else {
            strncpy(bi->type, "disk", sizeof(bi->type) - 1);
            bi->is_part = 0;
            bi->mountpoint[0] = '\0';
        }
    }
    closedir(d);

    /* Print parents first, then partitions */
    for (int i = 0; i < count; i++) {
        if (!list[i].is_part) {
            printf("%-10s %7s %2d %7s %2d %-6s %s\n",
                   list[i].name, list[i].maj_min, list[i].rm,
                   list[i].size_str, list[i].ro, list[i].type,
                   list[i].mountpoint);

            /* Check children */
            for (int j = 0; j < count; j++) {
                if (list[j].is_part && strncmp(list[j].name, list[i].name, strlen(list[i].name)) == 0) {
                    char child_name[40];
                    snprintf(child_name, sizeof(child_name), "└─%s", list[j].name);
                    printf("%-10s %7s %2d %7s %2d %-6s %s\n",
                           child_name, list[j].maj_min, list[j].rm,
                           list[j].size_str, list[j].ro, list[j].type,
                           list[j].mountpoint);
                }
            }
        }
    }

    return 0;
}
