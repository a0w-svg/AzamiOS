/* ============================================================================
 * AzamiOS — System Information Fetch Utility (fetch.elf)
 * File: userland/apps/fetch/main.c
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/sysinfo.h>
#include <sys/statvfs.h>

static void get_cpu_info(char *cpu_name, size_t max_len, int *cores)
{
    strncpy(cpu_name, "x86_64 Processor", max_len - 1);
    cpu_name[max_len - 1] = '\0';
    *cores = 1;

    FILE *f = fopen("/proc/cpuinfo", "r");
    if (!f) return;

    char line[256];
    int count = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "processor", 9) == 0) {
            count++;
        } else if (strncmp(line, "model name", 10) == 0) {
            char *colon = strchr(line, ':');
            if (colon) {
                colon++;
                while (*colon == ' ' || *colon == '\t') colon++;
                char *nl = strchr(colon, '\n');
                if (nl) *nl = '\0';
                strncpy(cpu_name, colon, max_len - 1);
                cpu_name[max_len - 1] = '\0';
            }
        }
    }
    fclose(f);
    if (count > 0) *cores = count;
}

static void get_uptime_str(char *out, size_t max_len)
{
    unsigned long sec = 0;
    FILE *f = fopen("/proc/uptime", "r");
    if (f) {
        char buf[64];
        if (fgets(buf, sizeof(buf), f)) {
            sec = (unsigned long)atoi(buf);
        }
        fclose(f);
    }
    if (sec == 0) {
        struct sysinfo si;
        if (sysinfo(&si) == 0) sec = (unsigned long)si.uptime;
    }

    unsigned long days = sec / 86400;
    unsigned long hours = (sec % 86400) / 3600;
    unsigned long mins = (sec % 3600) / 60;
    unsigned long s = sec % 60;

    if (days > 0) {
        snprintf(out, max_len, "%lud %luh %lum", days, hours, mins);
    } else if (hours > 0) {
        snprintf(out, max_len, "%luh %lum %lus", hours, mins, s);
    } else if (mins > 0) {
        snprintf(out, max_len, "%lum %lus", mins, s);
    } else {
        snprintf(out, max_len, "%lus", s);
    }
}

static void make_bar(char *out, size_t max_len, int pct, int bar_width)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    int filled = (pct * bar_width) / 100;

    size_t off = 0;
    if (off < max_len - 1) out[off++] = '[';
    for (int i = 0; i < bar_width && off < max_len - 4; i++) {
        if (i < filled) {
            /* UTF-8 full block: \xe2\x96\x88 */
            out[off++] = '#';
        } else {
            out[off++] = '-';
        }
    }
    if (off < max_len - 1) out[off++] = ']';
    out[off] = '\0';
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    /* Gather System Info */
    char hostname[64] = "azami";
    gethostname(hostname, sizeof(hostname));

    char user[32] = "root";
    char *env_user = getenv("USER");
    if (env_user && env_user[0]) strncpy(user, env_user, sizeof(user) - 1);

    char cpu_name[80];
    int cpu_cores = 1;
    get_cpu_info(cpu_name, sizeof(cpu_name), &cpu_cores);

    char uptime_str[64];
    get_uptime_str(uptime_str, sizeof(uptime_str));

    struct sysinfo si;
    memset(&si, 0, sizeof(si));
    sysinfo(&si);
    unsigned long unit = si.mem_unit ? si.mem_unit : 4096;
    unsigned long total_mb = (si.totalram * unit) / (1024 * 1024);
    unsigned long free_mb  = (si.freeram * unit) / (1024 * 1024);
    unsigned long used_mb  = total_mb > free_mb ? (total_mb - free_mb) : 0;
    int mem_pct = (total_mb > 0) ? (int)((used_mb * 100) / total_mb) : 0;

    char mem_bar[32];
    make_bar(mem_bar, sizeof(mem_bar), mem_pct, 10);

    /* Disk usage on / */
    unsigned long disk_total_mb = 0, disk_used_mb = 0;
    int disk_pct = 0;
    struct statvfs vfs;
    if (statvfs("/", &vfs) == 0 && vfs.f_blocks > 0) {
        unsigned long bsize = vfs.f_frsize ? vfs.f_frsize : vfs.f_bsize;
        disk_total_mb = (vfs.f_blocks * bsize) / (1024 * 1024);
        unsigned long avail_mb = (vfs.f_bavail * bsize) / (1024 * 1024);
        disk_used_mb = disk_total_mb > avail_mb ? (disk_total_mb - avail_mb) : 0;
        disk_pct = (disk_total_mb > 0) ? (int)((disk_used_mb * 100) / disk_total_mb) : 0;
    }

    char disk_bar[32];
    make_bar(disk_bar, sizeof(disk_bar), disk_pct, 10);

    /* Render Neofetch-style Dashboard */
    printf("\033[1;35m        /\\          \033[1;32m%s\033[0m@\033[1;34m%s\033[0m\n", user, hostname);
    printf("\033[1;35m       /  \\         \033[0;37m---------------------------------------\033[0m\n");
    printf("\033[1;35m      / /\\ \\        \033[1;36mOS:\033[0m        AzamiOS 7.0 (x86_64)\n");
    printf("\033[1;35m     / /  \\ \\       \033[1;36mKernel:\033[0m    7.0.0-posix (SMP Preemptive CFS)\n");
    printf("\033[1;35m    / / /\\ \\ \\      \033[1;36mUptime:\033[0m    %s\n", uptime_str);
    printf("\033[1;35m   / / /  \\ \\ \\     \033[1;36mCPU:\033[0m       %s (%d cores)\n", cpu_name, cpu_cores);
    printf("\033[1;35m  / / / /\\ \\ \\ \\    \033[1;36mMemory:\033[0m    %lu MB / %lu MB %s %d%%\n", used_mb, total_mb, mem_bar, mem_pct);
    if (disk_total_mb > 0) {
        printf("\033[1;35m /_/_/_/  \\_\\_\\_\\   \033[1;36mDisk (/):\033[0m  %lu MB / %lu MB %s %d%%\n", disk_used_mb, disk_total_mb, disk_bar, disk_pct);
    } else {
        printf("\033[1;35m /_/_/_/  \\_\\_\\_\\   \033[1;36mProcesses:\033[0m %d active tasks\n", (int)si.procs);
    }
    printf("                    \033[1;36mDisplay:\033[0m   1280x800x32bpp (azwm v2.0 VSync)\n");
    printf("                    \033[1;36mSecurity:\033[0m  SMEP, SMAP, UMIP, Canary, Yama, LinkGuard\n");
    printf("\n");
    printf("                    \033[41m   \033[42m   \033[43m   \033[44m   \033[45m   \033[46m   \033[47m   \033[0m\n");
    printf("\n");

    return 0;
}
