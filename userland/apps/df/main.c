/* ============================================================================
 * AzamiOS Userspace — Disk Free Space Utility (df.elf)
 * File: userland/apps/df/main.c
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/sys/statfs.h"

static void show_df(const char *dev, const char *mount_point)
{
    struct statfs sf;
    if (statfs(mount_point, &sf) == 0) {
        unsigned long long total_kb = (sf.f_blocks * sf.f_bsize) / 1024;
        unsigned long long free_kb = (sf.f_bfree * sf.f_bsize) / 1024;
        unsigned long long used_kb = total_kb > free_kb ? (total_kb - free_kb) : 0;
        int pct = total_kb > 0 ? (int)((used_kb * 100) / total_kb) : 0;

        printf("%-12s %10llu %10llu %10llu %4d%% %s\n",
               dev, total_kb, used_kb, free_kb, pct, mount_point);
    } else {
        printf("%-12s %10s %10s %10s    - %s\n", dev, "N/A", "N/A", "N/A", mount_point);
    }
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("%-12s %10s %10s %10s %5s %s\n",
           "Filesystem", "1K-blocks", "Used", "Available", "Use%", "Mounted on");

    show_df("ram0", "/");
    show_df("devfs", "/dev");
    show_df("sata0", "/hdd");

    return 0;
}
