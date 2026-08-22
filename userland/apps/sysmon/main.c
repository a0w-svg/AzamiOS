/* ============================================================================
 * AzamiOS — System Monitor & Task Manager (v4.0)
 * File: userland/apps/sysmon/main.c
 *
 * Features:
 *  • Real-time 4-Core SMP CPU telemetry bars & rolling history curves
 *  • Physical memory consumption meter + memory history curve
 *  • Storage meters for Root FS (/) and SATA Hard Drive (/hdd)
 *  • Active Process List with PID, name, state, and interactive "End Task"
 *  • Keyboard navigation (Up/Down, Del, k to terminate)
 *  • Catppuccin Mocha aesthetic with rounded cards
 * ============================================================================ */

#include <stdbool.h>
#include "../../libc/include/az/ipc.h"
#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/string.h"
#include "../../libc/include/dirent.h"
#include "../../libc/include/unistd.h"
#include "../../libc/include/sys/syscall.h"
#include "../../libc/include/sys/sysinfo.h"
#include "../../libc/include/sys/statfs.h"
#include "../azwm/protocol.h"
#include "../azwm/de_protocol.h"
#include "../azwm/de_font.h"
#include "../shared/ui_kit.h"

#define SERVER_CHAN  1
#define WIN_W       660
#define WIN_H       480
#define MAP_ADDR    ((void *)0x64000000)

static uk_window_t g_win;
static unsigned int g_tick = 0;

/* ── System Telemetry ───────────────────────────────────────────────────── */
typedef struct {
    unsigned long long idle_ticks[16];
    unsigned long long active_ticks[16];
} az_sysstat_t;

static az_sysstat_t g_last_stat;

/* Rolling history for CPU cores and memory */
#define HIST_LEN  36
static unsigned int g_cpu_hist[4][HIST_LEN];
static unsigned int g_mem_hist[HIST_LEN];
static unsigned int g_mem_used_kb = 0;
static unsigned int g_mem_total_kb = 0;

/* Storage stats */
static unsigned long g_root_used_mb = 0;
static unsigned long g_root_total_mb = 32;
static unsigned long g_hdd_used_mb = 0;
static unsigned long g_hdd_total_mb = 0;

/* Process listing */
typedef struct {
    int  pid;
    char name[32];
    char state[16];
} proc_item_t;

#define MAX_PROC_LIST 32
static proc_item_t g_proc_list[MAX_PROC_LIST];
static int g_proc_count = 0;
static int g_selected_proc = 0;
static int g_proc_scroll = 0;

static const unsigned int g_core_colors[4] = {
    UK_TEAL, UK_MAUVE, UK_BLUE, UK_PEACH
};

static void refresh_processes(void)
{
    DIR *dir = opendir("/proc");
    if (!dir) return;

    g_proc_count = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && g_proc_count < MAX_PROC_LIST) {
        if (ent->d_name[0] >= '0' && ent->d_name[0] <= '9') {
            int pid = 0;
            for (int k = 0; ent->d_name[k]; k++) {
                if (ent->d_name[k] >= '0' && ent->d_name[k] <= '9')
                    pid = pid * 10 + (ent->d_name[k] - '0');
            }
            g_proc_list[g_proc_count].pid = pid;
            strcpy(g_proc_list[g_proc_count].state, "Running");

            char stat_path[64];
            snprintf(stat_path, sizeof(stat_path), "/proc/%d/stat", pid);
            int fd = sys_open(stat_path, 0, 0);
            if (fd >= 0) {
                char sbuf[128];
                int nr = sys_read(fd, sbuf, sizeof(sbuf) - 1);
                if (nr > 0) {
                    sbuf[nr] = '\0';
                    char *p1 = strchr(sbuf, '(');
                    char *p2 = strrchr(sbuf, ')');
                    if (p1 && p2 && p2 > p1) {
                        int nlen = (int)(p2 - p1 - 1);
                        if (nlen > 31) nlen = 31;
                        strncpy(g_proc_list[g_proc_count].name, p1 + 1, nlen);
                        g_proc_list[g_proc_count].name[nlen] = '\0';
                    } else {
                        snprintf(g_proc_list[g_proc_count].name, 32, "pid_%d", pid);
                    }
                }
                sys_close(fd);
            } else {
                snprintf(g_proc_list[g_proc_count].name, 32, "process_%d", pid);
            }
            g_proc_count++;
        }
    }
    closedir(dir);
}

static void draw_bar_rounded(int x, int y, int w, int h, unsigned int val, unsigned int max,
                             unsigned int fg, unsigned int bg)
{
    uk_fill_rounded_rect(&g_win, x, y, w, h, h / 2, bg);
    if (max == 0) return;
    int fill_w = (int)(((unsigned long long)val * (unsigned long long)w) / max);
    if (fill_w > w) fill_w = w;
    if (fill_w > 4) {
        uk_fill_rounded_rect(&g_win, x, y, fill_w, h, h / 2, fg);
    }
}

static void draw_sparkline(int x, int y, int w, int h, const unsigned int *hist,
                           unsigned int max_val, unsigned int color)
{
    uk_fill_rect(&g_win, x, y, w, h, UK_SURFACE0);
    uk_draw_rounded_rect_outline(&g_win, x, y, w, h, 2, UK_SURFACE1);

    if (max_val == 0) max_val = 1;
    int prev_x = x;
    int prev_y = y + h - 2;

    for (int i = 0; i < HIST_LEN; i++) {
        int pt_x = x + (i * w) / (HIST_LEN - 1);
        unsigned int val = hist[(g_tick + i) % HIST_LEN];
        if (val > max_val) val = max_val;
        int pt_y = y + h - 2 - (int)((val * (h - 4)) / max_val);

        if (i > 0) {
            uk_line(&g_win, prev_x, prev_y, pt_x, pt_y, color);
        }
        prev_x = pt_x;
        prev_y = pt_y;
    }
}

static void draw_sysmon(void)
{
    unsigned int w = g_win.width;
    unsigned int h = g_win.height;

    uk_fill_rect(&g_win, 0, 0, (int)w, (int)h, UK_BASE);

    /* ── Title header ─────────────────────────────────────────────────────── */
    uk_gradient_h(&g_win, 0, 0, (int)w, 36, UK_MANTLE, UK_CRUST);
    uk_hline(&g_win, 0, 36, (int)w, UK_SURFACE0);
    uk_draw_badge(&g_win, 12, 8, "Monitor", UK_SURFACE1, UK_TEAL);
    uk_draw_text(&g_win, 80, 10, "System Performance & Resource Telemetry", UK_TEXT);

    int py = 44;

    /* ── Left Column: CPU, RAM, and Storage Meters (Width: 280px) ─────────── */
    uk_draw_section_header(&g_win, 12, py, 280, "CPU Core Activity", UK_TEAL);
    py += 22;

    for (int c = 0; c < 4; c++) {
        char core_str[32];
        unsigned int cur_cpu = g_cpu_hist[c][(g_tick + HIST_LEN - 1) % HIST_LEN];
        snprintf(core_str, sizeof(core_str), "Core %d: %3u%%", c, cur_cpu);
        uk_draw_text(&g_win, 12, py, core_str, UK_SUBTEXT0);
        draw_bar_rounded(110, py + 2, 180, 10, cur_cpu, 100, g_core_colors[c], UK_SURFACE0);
        py += 16;
    }

    draw_sparkline(12, py, 280, 30, g_cpu_hist[0], 100, UK_TEAL);
    py += 36;

    /* Memory Section */
    uk_draw_section_header(&g_win, 12, py, 280, "Physical Memory", UK_LAVENDER);
    py += 22;

    char mem_str[48];
    snprintf(mem_str, sizeof(mem_str), "%u MB / %u MB Used", g_mem_used_kb / 1024, g_mem_total_kb / 1024);
    uk_draw_text(&g_win, 12, py, mem_str, UK_SUBTEXT0);
    py += 16;
    draw_bar_rounded(12, py, 280, 12, g_mem_used_kb, g_mem_total_kb, UK_LAVENDER, UK_SURFACE0);
    py += 18;
    draw_sparkline(12, py, 280, 26, g_mem_hist, g_mem_total_kb, UK_LAVENDER);
    py += 32;

    /* Storage Section */
    uk_draw_section_header(&g_win, 12, py, 280, "Storage Drives", UK_SAPPHIRE);
    py += 22;

    char root_str[48];
    snprintf(root_str, sizeof(root_str), "/ (Initrd Ext2): %lu MB / %lu MB", g_root_used_mb, g_root_total_mb);
    uk_draw_text(&g_win, 12, py, root_str, UK_SUBTEXT0);
    py += 14;
    draw_bar_rounded(12, py, 280, 8, (unsigned int)g_root_used_mb, (unsigned int)g_root_total_mb, UK_SAPPHIRE, UK_SURFACE0);
    py += 14;

    if (g_hdd_total_mb > 0) {
        char hdd_str[48];
        snprintf(hdd_str, sizeof(hdd_str), "/hdd (SATA Drive): %lu MB / %lu MB", g_hdd_used_mb, g_hdd_total_mb);
        uk_draw_text(&g_win, 12, py, hdd_str, UK_SUBTEXT0);
        py += 14;
        draw_bar_rounded(12, py, 280, 8, (unsigned int)g_hdd_used_mb, (unsigned int)g_hdd_total_mb, UK_GREEN, UK_SURFACE0);
    }

    /* ── Right Column: Process List (Start at x: 310) ─────────────────────── */
    int rx = 310;
    int ry = 44;
    uk_vline(&g_win, rx - 10, 36, (int)h - 70, UK_SURFACE1);

    uk_draw_section_header(&g_win, rx, ry, (int)w - rx - 12, "Active Tasks & Processes", UK_YELLOW);
    ry += 22;

    /* Table Headers */
    uk_fill_rect(&g_win, rx, ry, (int)w - rx - 12, 22, UK_SURFACE0);
    uk_draw_text(&g_win, rx + 8,  ry + 3, "PID", UK_SUBTEXT0);
    uk_draw_text(&g_win, rx + 48, ry + 3, "Name", UK_SUBTEXT0);
    uk_draw_text(&g_win, rx + 240, ry + 3, "State", UK_SUBTEXT0);
    ry += 24;

    int max_rows = 13;
    for (int r = 0; r < max_rows; r++) {
        int idx = r + g_proc_scroll;
        if (idx >= g_proc_count) break;

        int row_y = ry + r * 22;
        unsigned int row_bg = (idx == g_selected_proc) ? UK_SURFACE1 : UK_BASE;
        uk_fill_rect(&g_win, rx, row_y, (int)w - rx - 12, 21, row_bg);

        if (idx == g_selected_proc) {
            uk_fill_rect(&g_win, rx, row_y, 3, 21, UK_YELLOW);
        }

        char pid_buf[16];
        snprintf(pid_buf, sizeof(pid_buf), "%d", g_proc_list[idx].pid);
        unsigned int fg = (idx == g_selected_proc) ? UK_TEXT : UK_SUBTEXT1;

        uk_draw_text(&g_win, rx + 8,  row_y + 3, pid_buf, fg);
        uk_draw_text_clip(&g_win, rx + 48, row_y + 3, g_proc_list[idx].name, fg, 184);
        uk_draw_text(&g_win, rx + 240, row_y + 3, g_proc_list[idx].state, UK_GREEN);
    }

    /* ── Bottom Action Bar ────────────────────────────────────────────────── */
    uk_fill_rect(&g_win, 0, (int)h - 34, (int)w, 34, UK_SURFACE0);
    uk_hline(&g_win, 0, (int)h - 34, (int)w, UK_SURFACE1);

    char foot[80];
    snprintf(foot, sizeof(foot), "Tasks: %d  |  [k / Del] End Task  |  Uptime: %u sec", g_proc_count, g_tick);
    uk_draw_text(&g_win, 12, (int)h - 24, foot, UK_OVERLAY0);

    /* End Process Button */
    uk_draw_button(&g_win, (int)w - 110, (int)h - 29, 98, 24, "End Task", UK_BTN_PRESSED);

    uk_invalidate(&g_win);
}

static void kill_selected_process(void)
{
    if (g_selected_proc >= 0 && g_selected_proc < g_proc_count) {
        int pid = g_proc_list[g_selected_proc].pid;
        if (pid > 1) {
            sys_kill(pid, 9 /* SIGKILL */);
            refresh_processes();
            draw_sysmon();
        }
    }
}

static void update_telemetry(void)
{
    struct sysinfo info;
    sysinfo(&info);
    g_mem_total_kb = (unsigned int)((info.totalram * info.mem_unit) / 1024);
    g_mem_used_kb = (unsigned int)(((info.totalram - info.freeram) * info.mem_unit) / 1024);

    unsigned int midx = g_tick % HIST_LEN;
    g_mem_hist[midx] = g_mem_used_kb;

    /* Query storage capacity */
    struct statfs sfs;
    if (statfs("/", &sfs) == 0 && sfs.f_blocks > 0) {
        g_root_total_mb = (unsigned long)((sfs.f_blocks * sfs.f_bsize) / (1024 * 1024));
        g_root_used_mb  = (unsigned long)(((sfs.f_blocks - sfs.f_bfree) * sfs.f_bsize) / (1024 * 1024));
    }
    if (statfs("/hdd", &sfs) == 0 && sfs.f_blocks > 0) {
        g_hdd_total_mb = (unsigned long)((sfs.f_blocks * sfs.f_bsize) / (1024 * 1024));
        g_hdd_used_mb  = (unsigned long)(((sfs.f_blocks - sfs.f_bfree) * sfs.f_bsize) / (1024 * 1024));
    }

    az_sysstat_t cur_stat;
    syscall1(SYS_AZ_SYSSTAT, (long)&cur_stat);

    for (int cc = 0; cc < 4; cc++) {
        unsigned long long cur_idle = cur_stat.idle_ticks[cc];
        unsigned long long cur_active = cur_stat.active_ticks[cc];
        unsigned long long last_idle = g_last_stat.idle_ticks[cc];
        unsigned long long last_active = g_last_stat.active_ticks[cc];

        unsigned long long d_idle = cur_idle - last_idle;
        unsigned long long d_active = cur_active - last_active;
        unsigned long long total = d_idle + d_active;

        unsigned int usage = 0;
        if (total > 0) {
            usage = (unsigned int)((d_active * 100ULL) / total);
        }
        if (usage > 100) usage = 100;

        g_cpu_hist[cc][midx] = usage;
    }
    g_last_stat = cur_stat;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    puts("[sysmon] Starting v4.0...");

    syscall1(SYS_AZ_SYSSTAT, (long)&g_last_stat);

    update_telemetry();
    for (int c = 0; c < 4; c++)
        for (int i = 0; i < HIST_LEN; i++)
            g_cpu_hist[c][i] = g_cpu_hist[c][0];

    for (int i = 0; i < HIST_LEN; i++)
        g_mem_hist[i] = g_mem_used_kb;

    az_fb_info_t fb;
    unsigned int sw = 1280, sh = 800;
    if (az_fb_info(&fb) == 0 && fb.width > 0 && fb.height > 0) {
        sw = fb.width;
        sh = fb.height;
    }

    int ret = uk_window_connect(&g_win, "System Monitor",
                                (int)(sw / 2) - WIN_W / 2,
                                (int)(sh / 2) - WIN_H / 2,
                                WIN_W, WIN_H, MAP_ADDR, SERVER_CHAN);
    if (ret < 0) {
        puts("[sysmon] FATAL: Failed to connect window");
        return -1;
    }

    refresh_processes();
    draw_sysmon();

    /* Register autonomous 1-second timer tick */
    az_set_timer(g_win.client_chan, 1000, 0);

    az_ipc_msg_t raw_msg;
    az_wm_msg_t *msg = (az_wm_msg_t *)&raw_msg;

    for (;;) {
        if (az_channel_recv(g_win.client_chan, &raw_msg) != 0) continue;

        switch (msg->type) {
        case AZ_WM_TIMER_TICK:
            g_tick++;
            update_telemetry();
            refresh_processes();
            draw_sysmon();
            break;

        case AZ_WM_KEY_EVENT: {
            if (!msg->key.pressed) break;

            if (msg->key.keycode == 'q' || msg->key.keycode == 0x1B) { /* ESC or q */
                sys_exit(0);
            } else if (msg->key.keycode == 'k' || msg->key.keycode == 127) { /* Delete or k */
                kill_selected_process();
            } else if (msg->key.keycode == 0xE2) { /* Up */
                if (g_selected_proc > 0) {
                    g_selected_proc--;
                    if (g_selected_proc < g_proc_scroll) g_proc_scroll = g_selected_proc;
                    draw_sysmon();
                }
            } else if (msg->key.keycode == 0xE3) { /* Down */
                if (g_selected_proc < g_proc_count - 1) {
                    g_selected_proc++;
                    if (g_selected_proc >= g_proc_scroll + 13) g_proc_scroll = g_selected_proc - 12;
                    draw_sysmon();
                }
            }
            break;
        }

        case AZ_WM_MOUSE_EVENT: {
            int mx = msg->mouse.abs_x;
            int my = msg->mouse.abs_y;
            if (msg->mouse.buttons & AZ_MOUSE_BTN_LEFT) {
                /* End Task Button */
                if (mx >= (int)g_win.width - 110 && mx <= (int)g_win.width - 12 &&
                    my >= (int)g_win.height - 29 && my <= (int)g_win.height - 5) {
                    kill_selected_process();
                    break;
                }

                /* Process List Selection */
                int rx = 310;
                int ry = 68;
                if (mx >= rx && mx <= (int)g_win.width - 12 && my >= ry) {
                    int clicked = (my - ry) / 22 + g_proc_scroll;
                    if (clicked >= 0 && clicked < g_proc_count) {
                        g_selected_proc = clicked;
                        draw_sysmon();
                    }
                }
            }
            break;
        }

        default:
            break;
        }
    }

    return 0;
}
