/* ============================================================================
 * AzamiOS Userspace — Disk Free Space Utility (df.elf)
 * File: userland/apps/df/main.c
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/statfs.h>

static void show_df(const char *dev, const char *mount_point)
{
    struct statfs sf;
    if (statfs(mount_point, &sf) == 0) {
        unsigned long long total_kb = (sf.f_blocks * sf.f_bsize) / 1024;
        unsigned long long free_kb = (sf.f_bfree * sf.f_bsize) / 1024;
        unsigned long long used_kb = total_kb > free_kb ? (total_kb - free_kb) : 0;
        int pct = total_kb > 0 ? (int)((used_kb * 100) / total_kb) : 0;

        printf("%-14s %10llu %10llu %10llu %4d%% %s\n",
               dev, total_kb, used_kb, free_kb, pct, mount_point);
    } else {
        printf("%-14s %10s %10s %10s    - %s\n", dev, "N/A", "N/A", "N/A", mount_point);
    }
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("%-14s %10s %10s %10s %5s %s\n",
           "Filesystem", "1K-blocks", "Used", "Available", "Use%", "Mounted on");

    int fd = open("/proc/mounts", O_RDONLY);
    if (fd >= 0) {
        char buf[2048];
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n > 0) {
            buf[n] = '\0';
            char *line = buf;
            while (*line) {
                while (*line == ' ' || *line == '\t') line++;
                if (*line == '\n' || *line == '\r' || *line == '#') {
                    line = strchr(line, '\n');
                    if (!line) break;
                    line++;
                    continue;
                }
                char *next = strchr(line, '\n');
                if (next) *next = '\0';

                char dev[64] = {0};
                char mnt[64] = {0};
                if (sscanf(line, "%63s %63s", dev, mnt) == 2) {
                    show_df(dev, mnt);
                }

                if (!next) break;
                line = next + 1;
            }
            return 0;
        }
    }

    /* Fallback if /proc/mounts not available */
    show_df("/dev/sata0p2", "/");
    show_df("/dev/sata0p1", "/boot");
    show_df("devfs", "/dev");
    show_df("procfs", "/proc");

    return 0;
}
