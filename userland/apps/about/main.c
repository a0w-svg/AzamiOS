/* ============================================================================
 * AzamiOS — About AzamiOS
 * File: userland/apps/about/main.c
 * ============================================================================ */
#include "../../libc/include/az/ipc.h"
#include "../../libc/include/stdio.h"
#include "../../libc/include/string.h"
#include "../azwm/protocol.h"
#include "../azwm/de_protocol.h"
#include "../azwm/de_font.h"
#include "../shared/ui_kit.h"

#define SERVER_CHAN  1
#define WIN_W       520
#define WIN_H       360
#define MAP_ADDR    ((void *)0x61000000)

static uk_window_t g_win;

static void draw_about(void)
{
    unsigned int w = g_win.width;
    unsigned int h = g_win.height;

    /* Background */
    uk_gradient_v(&g_win, 0, 0, (int)w, (int)h, UK_BASE, UK_MANTLE);

    /* ── Top accent banner ─────────────────────────────────────────────── */
    uk_gradient_h(&g_win, 0, 0, (int)w, 90, UK_MAUVE, UK_LAVENDER);

    /* AzamiOS logo (2× scale, white) */
    uk_draw_text_2x(&g_win, (int)w / 2 - 7 * 8, 16, "AzamiOS", UK_BASE);

    /* Shadow */
    uk_draw_text_2x(&g_win, (int)w / 2 - 7 * 8 + 1, 17, "AzamiOS", 0x44000000);

    /* Version */
    uk_draw_text_centred(&g_win, (int)w / 2, 56, "Version 7.0  —  x86_64 Microkernel", UK_BASE);

    /* ── Info panels ───────────────────────────────────────────────────── */
    int py = 104;
    int pw = (int)w - 48;
    int px = 24;

    /* OS section */
    uk_draw_panel(&g_win, px, py, pw, 44, UK_SURFACE0);
    uk_fill_rect(&g_win, px, py, 4, 44, UK_MAUVE);
    uk_draw_text(&g_win, px + 14, py + 6,  "Operating System", UK_SUBTEXT0);
    uk_draw_text(&g_win, px + 14, py + 22, "AzamiOS v7.0 — Modular Microkernel Architecture", UK_TEXT);
    py += 52;

    /* Architecture */
    uk_draw_panel(&g_win, px, py, pw, 44, UK_SURFACE0);
    uk_fill_rect(&g_win, px, py, 4, 44, UK_BLUE);
    uk_draw_text(&g_win, px + 14, py + 6,  "Architecture", UK_SUBTEXT0);
    uk_draw_text(&g_win, px + 14, py + 22, "x86_64  |  Ring-0 Kernel  |  Ring-3 Userspace", UK_TEXT);
    py += 52;

    /* Display */
    az_fb_info_t fb;
    char res_str[64];
    if (az_fb_info(&fb) == 0) {
        /* Build "WIDTHxHEIGHT" string manually */
        unsigned int wv = fb.width, hv = fb.height;
        int i = 0;
        unsigned int tmp;
        char tbuf[12];
        int ti = 0;
        /* width */
        if (wv == 0) { tbuf[ti++] = '0'; } else {
            tmp = wv; int d=0; while(tmp){d++;tmp/=10;}
            tmp=wv; for(int k=d-1;k>=0;k--){tbuf[ti+k]='0'+(char)(tmp%10);tmp/=10;} ti+=d;
        }
        tbuf[ti++] = 'x';
        /* height */
        if (hv == 0) { tbuf[ti++] = '0'; } else {
            tmp = hv; int d=0; while(tmp){d++;tmp/=10;}
            tmp=hv; for(int k=d-1;k>=0;k--){tbuf[ti+k]='0'+(char)(tmp%10);tmp/=10;} ti+=d;
        }
        tbuf[ti] = '\0';
        const char *pre = "Framebuffer: ";
        for (i = 0; pre[i]; i++) res_str[i] = pre[i];
        for (int j = 0; tbuf[j]; j++) res_str[i++] = tbuf[j];
        const char *suf = "  |  32bpp ARGB";
        for (int j = 0; suf[j]; j++) res_str[i++] = suf[j];
        res_str[i] = '\0';
    } else {
        const char *fb = "Framebuffer: 1280x800 (default)";
        int j;
        for (j = 0; fb[j]; j++) res_str[j] = fb[j];
        res_str[j] = '\0';
    }
    uk_draw_panel(&g_win, px, py, pw, 44, UK_SURFACE0);
    uk_fill_rect(&g_win, px, py, 4, 44, UK_TEAL);
    uk_draw_text(&g_win, px + 14, py + 6,  "Display", UK_SUBTEXT0);
    uk_draw_text(&g_win, px + 14, py + 22, res_str, UK_TEXT);
    py += 52;

    /* Compositor */
    uk_draw_panel(&g_win, px, py, pw, 44, UK_SURFACE0);
    uk_fill_rect(&g_win, px, py, 4, 44, UK_PEACH);
    uk_draw_text(&g_win, px + 14, py + 6,  "Window Manager", UK_SUBTEXT0);
    uk_draw_text(&g_win, px + 14, py + 22, "azwm  |  IPC-based compositor  |  Shared memory", UK_TEXT);

    /* ── Footer ────────────────────────────────────────────────────────── */
    uk_hline(&g_win, 0, (int)h - 28, (int)w, UK_SURFACE1);
    uk_draw_text_centred(&g_win, (int)w / 2, (int)h - 20,
                         "Copyright 2026 AzamiOS Contributors — MIT License", UK_OVERLAY0);

    uk_invalidate(&g_win);
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    de_log("[about] Starting...");

    az_fb_info_t fb;
    unsigned int sw = 1280, sh = 800;
    if (az_fb_info(&fb) == 0) { sw = fb.width; sh = fb.height; }

    int ret = uk_window_connect(&g_win, "About AzamiOS",
                                (int)(sw / 2) - WIN_W / 2,
                                (int)(sh / 2) - WIN_H / 2,
                                WIN_W, WIN_H, MAP_ADDR, SERVER_CHAN);
    if (ret < 0) { de_log("[about] FATAL: window connect"); return -1; }

    draw_about();

    for (;;) {
        az_wm_msg_t msg;
        int r = az_channel_recv(g_win.client_chan, (az_ipc_msg_t *)&msg);
        if (r < 0) break;
        if (r != 0) continue;
        if (msg.type == AZ_WM_DESTROY_WINDOW) break;
    }
    sys_exit(0);
}
