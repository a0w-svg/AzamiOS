/* ============================================================================
 * AzamiOS — System Monitor
 * File: userland/apps/sysmon/main.c
 * ============================================================================ */
#include "../../libc/include/az/ipc.h"
#include "../../libc/include/stdio.h"
#include "../../libc/include/string.h"
#include "../azwm/protocol.h"
#include "../azwm/de_protocol.h"
#include "../azwm/de_font.h"
#include "../shared/ui_kit.h"

#define SERVER_CHAN  1
#define WIN_W       540
#define WIN_H       380
#define MAP_ADDR    ((void *)0x64000000)

static uk_window_t g_win;
static unsigned int g_tick = 0;

#include "../../libc/include/sys/syscall.h"
#include "../../libc/include/sys/sysinfo.h"

/* ── System Telemetry ───────────────────────────────────────────────────── */
typedef struct {
    unsigned long long idle_ticks[16];
    unsigned long long active_ticks[16];
} az_sysstat_t;

static az_sysstat_t g_last_stat;

/* Rolling history for 4 CPU cores (bar graph) */
#define HIST_LEN  32
static unsigned int g_cpu_hist[4][HIST_LEN];
static unsigned int g_mem_used_kb = 0;
static unsigned int g_mem_total_kb = 0;
static unsigned int g_procs = 0;

static void uint_to_str(unsigned int v, char *buf, int maxlen)
{
    if (v == 0) { buf[0]='0'; buf[1]='\0'; return; }
    char tmp[16]; int i = 0;
    while (v && i < 15) { tmp[i++] = '0' + (char)(v % 10); v /= 10; }
    int j = 0;
    while (i > 0 && j < maxlen - 1) buf[j++] = tmp[--i];
    buf[j] = '\0';
}

static void draw_bar(int bx, int by, int bw, int bh,
                     unsigned int val, unsigned int maxval,
                     unsigned int fill_col, unsigned int bg_col)
{
    uk_fill_rect(&g_win, bx, by, bw, bh, bg_col);
    if (maxval > 0) {
        int filled = (int)((unsigned long)val * (unsigned long)bw / maxval);
        if (filled > bw) filled = bw;
        uk_fill_rect(&g_win, bx, by, filled, bh, fill_col);
    }
    /* Border */
    uk_hline(&g_win, bx, by, bw, UK_SURFACE2);
    uk_hline(&g_win, bx, by + bh - 1, bw, UK_SURFACE2);
}

static void draw_sparkline(int gx, int gy, int gw, int gh,
                           unsigned int *hist, unsigned int maxval,
                           unsigned int line_col)
{
    uk_fill_rect(&g_win, gx, gy, gw, gh, UK_SURFACE0);
    uk_hline(&g_win, gx, gy + gh - 1, gw, UK_SURFACE1);

    int i;
    for (i = 0; i < HIST_LEN - 1; i++) {
        unsigned int idx0 = (g_tick + 1 + i) % HIST_LEN;
        unsigned int idx1 = (g_tick + 2 + i) % HIST_LEN;
        unsigned int v0 = hist[idx0];
        unsigned int v1 = hist[idx1];
        if (maxval == 0) continue;
        int y0 = gy + gh - 1 - (int)(v0 * (unsigned int)(gh - 1) / maxval);
        int y1 = gy + gh - 1 - (int)(v1 * (unsigned int)(gh - 1) / maxval);
        if (y0 < gy) y0 = gy;
        if (y1 < gy) y1 = gy;
        int x0 = gx + i * gw / HIST_LEN;
        int x1 = gx + (i + 1) * gw / HIST_LEN;
        /* Simple vertical line between y0 and y1 at x0 */
        int ylo = (y0 < y1) ? y0 : y1;
        int yhi = (y0 > y1) ? y0 : y1;
        int yy;
        for (yy = ylo; yy <= yhi; yy++)
            uk_put_pixel(&g_win, x0, yy, line_col);
        /* Fill under line */
        int fill_y0 = (y0 < gy) ? gy : y0;
        for (yy = fill_y0; yy < gy + gh; yy++)
            uk_put_pixel(&g_win, x0, yy,
                         uk_blend(UK_SURFACE0, line_col, 40));
        (void)x1;
    }
}

static void draw_sysmon(void)
{
    unsigned int w = g_win.width;
    unsigned int h = g_win.height;

    uk_fill_rect(&g_win, 0, 0, (int)w, (int)h, UK_BASE);

    /* Header */
    uk_gradient_h(&g_win, 0, 0, (int)w, 44, UK_SURFACE0, UK_BASE);
    uk_fill_rect(&g_win, 0, 0, 4, 44, UK_TEAL);
    uk_draw_text(&g_win, 16, 6,  "System Monitor", UK_TEXT);
    uk_draw_text(&g_win, 16, 24, "AzamiOS v7.0 — x86_64", UK_OVERLAY0);
    uk_hline(&g_win, 0, 44, (int)w, UK_SURFACE1);

    int py = 56;

    /* ── CPU Section ────────────────────────────────────────────────────── */
    uk_draw_section_header(&g_win, 12, py, (int)w - 24, "CPU", UK_TEAL);
    py += 28;

    /* 4 Core bars */
    int c;
    for (c = 0; c < 4; c++) {
        char label[16];
        label[0]='C'; label[1]='o'; label[2]='r'; label[3]='e'; label[4]=' ';
        label[5]='0' + (char)c; label[6]='\0';

        unsigned int usage = g_cpu_hist[c][(g_tick % HIST_LEN)];
        char pct[16];
        uint_to_str(usage, pct, sizeof(pct));
        /* append % */
        int pi = 0; while(pct[pi]) pi++;
        pct[pi]='%'; pct[pi+1]='\0';

        uk_draw_text(&g_win, 12, py, label, UK_SUBTEXT0);
        draw_bar(80, py, (int)w - 140, 12, usage, 100, UK_TEAL, UK_SURFACE0);
        uk_draw_text(&g_win, (int)w - 52, py, pct, UK_TEAL);
        py += 20;
    }

    /* Sparkline for Core 0 */
    draw_sparkline(12, py, (int)w - 24, 40,
                   g_cpu_hist[0], 100, UK_TEAL);
    py += 48;

    /* ── Memory Section ─────────────────────────────────────────────────── */
    uk_draw_section_header(&g_win, 12, py, (int)w - 24, "Memory", UK_LAVENDER);
    py += 28;

    /* Mem bar */
    char mem_str[32];
    unsigned int used_mb = g_mem_used_kb / 1024;
    unsigned int total_mb = g_mem_total_kb / 1024;
    char ub[8], tb[8];
    uint_to_str(used_mb, ub, 8);
    uint_to_str(total_mb, tb, 8);
    int i = 0; int j;
    for (j=0;ub[j];j++) mem_str[i++]=ub[j];
    mem_str[i++]=' '; mem_str[i++]='M'; mem_str[i++]='B';
    mem_str[i++]=' '; mem_str[i++]='/'; mem_str[i++]=' ';
    for (j=0;tb[j];j++) mem_str[i++]=tb[j];
    mem_str[i++]=' '; mem_str[i++]='M'; mem_str[i++]='B';
    mem_str[i]='\0';

    uk_draw_text(&g_win, 12, py, mem_str, UK_SUBTEXT0);
    py += 20;
    draw_bar(12, py, (int)w - 24, 18,
             g_mem_used_kb, g_mem_total_kb,
             UK_LAVENDER, UK_SURFACE0);
    py += 28;

    /* ── Display info ───────────────────────────────────────────────────── */
    uk_draw_section_header(&g_win, 12, py, (int)w - 24, "Display", UK_BLUE);
    py += 28;

    az_fb_info_t fb;
    if (az_fb_info(&fb) == 0) {
        char ws[8], hs[8];
        uint_to_str(fb.width, ws, 8);
        uint_to_str(fb.height, hs, 8);
        char fbstr[32];
        int k = 0; int jj;
        for (jj=0;ws[jj];jj++) fbstr[k++]=ws[jj];
        fbstr[k++]='x';
        for (jj=0;hs[jj];jj++) fbstr[k++]=hs[jj];
        fbstr[k++]=' '; fbstr[k++]='@'; fbstr[k++]=' '; fbstr[k++]='3'; fbstr[k++]='2';
        fbstr[k++]='b'; fbstr[k++]='p'; fbstr[k++]='p'; fbstr[k]='\0';
        uk_draw_text(&g_win, 12, py, fbstr, UK_TEXT);
    } else {
        uk_draw_text(&g_win, 12, py, "1280x800 @ 32bpp (default)", UK_TEXT);
    }
    py += 28;

    /* ── Processes ──────────────────────────────────────────────────────── */
    uk_draw_section_header(&g_win, 12, py, (int)w - 24, "Processes", UK_YELLOW);
    py += 28;

    char proc_str[32];
    const char *p_pre = "Active Tasks: ";
    int p_idx = 0;
    for (; p_pre[p_idx]; p_idx++) proc_str[p_idx] = p_pre[p_idx];
    char p_val[12];
    uint_to_str(g_procs, p_val, 12);
    for (int p_j = 0; p_val[p_j]; p_j++) proc_str[p_idx++] = p_val[p_j];
    proc_str[p_idx] = '\0';
    uk_draw_text(&g_win, 12, py, proc_str, UK_TEXT);
    py += 28;

    /* Tick counter */
    char tstr[24];
    const char *tpre = "Uptime ticks: ";
    int tp = 0;
    for (;tpre[tp];tp++) tstr[tp] = tpre[tp];
    char tv[12];
    uint_to_str(g_tick, tv, 12);
    for (j=0;tv[j];j++) tstr[tp++]=tv[j];
    tstr[tp]='\0';
    uk_draw_text(&g_win, 12, (int)h - 22, tstr, UK_OVERLAY0);

    uk_invalidate(&g_win);
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    puts("[sysmon] Starting...");


    /* Initialise stats */
    int c, i;
    for (c = 0; c < 4; c++)
        for (i = 0; i < HIST_LEN; i++)
            g_cpu_hist[c][i] = 0;

    syscall1(SYS_AZ_SYSSTAT, (long)&g_last_stat);

    az_fb_info_t fb;
    unsigned int sw = 1280, sh = 800;
    if (az_fb_info(&fb) == 0) { sw = fb.width; sh = fb.height; }

    int ret = uk_window_connect(&g_win, "System Monitor",
                                (int)(sw / 2) - WIN_W / 2,
                                (int)(sh / 2) - WIN_H / 2,
                                WIN_W, WIN_H, MAP_ADDR, SERVER_CHAN);
    if (ret < 0) { puts("[sysmon] FATAL"); return -1; }

    draw_sysmon();

    /* Register autonomous 1-second timer tick (Bug 10 fix) */
    az_set_timer(g_win.client_chan, 1000, 0);

    for (;;) {
        az_wm_msg_t msg;
        int r = az_channel_recv(g_win.client_chan, (az_ipc_msg_t *)&msg);
        if (r < 0) break;
        if (r != 0) continue;

        if (msg.type == AZ_WM_DESTROY_WINDOW) {
            break;
        }

        if (msg.type == AZ_WM_TIMER_TICK) {
            g_tick++;

            /* ── Update Real Telemetry ──────────────────────────────────────── */
            struct sysinfo info;
            sysinfo(&info);
            g_mem_total_kb = (info.totalram * info.mem_unit) / 1024;
            g_mem_used_kb = ((info.totalram - info.freeram) * info.mem_unit) / 1024;
            g_procs = info.procs;

            az_sysstat_t cur_stat;
            syscall1(SYS_AZ_SYSSTAT, (long)&cur_stat);

            int cc;
            for (cc = 0; cc < 4; cc++) {
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
                
                unsigned int idx = g_tick % HIST_LEN;
                g_cpu_hist[cc][idx] = usage;
            }
            g_last_stat = cur_stat;

            draw_sysmon();
        }
    }
    sys_exit(0);
}
