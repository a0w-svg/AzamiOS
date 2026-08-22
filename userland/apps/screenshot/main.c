/* ============================================================================
 * AzamiOS Desktop Environment — Screenshot Utility
 * File: userland/apps/screenshot/main.c
 * ============================================================================ */
#include "../../libc/include/az/ipc.h"
#include "../../libc/include/stdio.h"
#include "../../libc/include/string.h"
#include "../../libc/include/unistd.h"
#include "../../libc/include/fcntl.h"
#include "../azwm/protocol.h"
#include "../azwm/de_protocol.h"
#include "../azwm/de_font.h"
#include "../shared/ui_kit.h"
#include "../shared/de_log.h"

#define SERVER_CHAN     1
#define WIN_W          480
#define WIN_H          320
#define MAP_ADDR       ((void *)0x63000000)

static uk_window_t g_win;
static char g_status_msg[128] = "Screen captured successfully!";
static int  g_saved = 0;

static int save_screenshot_ppm(const char *filename)
{
    az_fb_info_t fb;
    if (az_fb_info(&fb) != 0 || fb.width == 0 || fb.height == 0) {
        return -1;
    }

    /* Ensure framebuffer is mapped at 0x40000000 */
    az_fb_map((void *)0x40000000);

    int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return -1;

    char header[64];
    int hlen = snprintf(header, sizeof(header), "P6\n%u %u\n255\n", fb.width, fb.height);
    write(fd, header, hlen);

    /* Write RGB bytes row by row */
    static unsigned char row_buf[3840 * 3];
    unsigned int *src = (unsigned int *)0x40000000;

    for (unsigned int y = 0; y < fb.height; y++) {
        unsigned int *line = src + (y * (fb.pitch / 4));
        int bpos = 0;
        for (unsigned int x = 0; x < fb.width; x++) {
            unsigned int c = line[x];
            row_buf[bpos++] = (c >> 16) & 0xFF; /* Red */
            row_buf[bpos++] = (c >> 8) & 0xFF;  /* Green */
            row_buf[bpos++] = c & 0xFF;         /* Blue */
        }
        write(fd, row_buf, bpos);
    }

    close(fd);
    return 0;
}

static void draw_screenshot_ui(void)
{
    unsigned int w = g_win.width;
    unsigned int h = g_win.height;

    uk_gradient_v(&g_win, 0, 0, (int)w, (int)h, UK_BASE, UK_MANTLE);

    /* Header */
    uk_gradient_h(&g_win, 0, 0, (int)w, 50, UK_SURFACE0, UK_MANTLE);
    uk_fill_rounded_rect(&g_win, 12, 10, 30, 30, 8, UK_SAPPHIRE);
    uk_draw_text(&g_win, 18, 17, "[o]", UK_BASE);
    uk_draw_text_2x(&g_win, 52, 12, "Screenshot Tool", UK_TEXT);
    uk_hline(&g_win, 0, 50, (int)w, UK_SURFACE1);

    /* Info card */
    uk_draw_panel(&g_win, 24, 70, (int)w - 48, 140, UK_SURFACE0);

    az_fb_info_t fb;
    unsigned int sw = 1280, sh = 800;
    if (az_fb_info(&fb) == 0 && fb.width > 0) {
        sw = fb.width;
        sh = fb.height;
    }

    char res_buf[64];
    snprintf(res_buf, sizeof(res_buf), "Screen Resolution: %u x %u (32 bpp)", sw, sh);
    uk_draw_text(&g_win, 40, 86, res_buf, UK_SUBTEXT1);

    uk_draw_text(&g_win, 40, 110, "Target Output: /screenshot.ppm (P6 Netpbm Format)", UK_SUBTEXT0);

    /* Status badge */
    if (g_saved) {
        uk_fill_rounded_rect(&g_win, 40, 140, (int)w - 88, 36, 8, 0xFF2A3E34);
        uk_draw_text(&g_win, 54, 150, "Saved to /screenshot.ppm", UK_GREEN);
    } else {
        uk_fill_rounded_rect(&g_win, 40, 140, (int)w - 88, 36, 8, UK_SURFACE1);
        uk_draw_text(&g_win, 54, 150, g_status_msg, UK_TEXT);
    }

    /* Buttons */
    uk_draw_button(&g_win, 24, (int)h - 55, 140, 34, "Save File", UK_BTN_HOVER);
    uk_draw_button(&g_win, 180, (int)h - 55, 130, 34, "Retake", UK_BTN_NORMAL);
    uk_draw_button(&g_win, (int)w - 120, (int)h - 55, 96, 34, "Close", UK_BTN_NORMAL);

    uk_invalidate(&g_win);
}

int main(int argc, char **argv)
{
    de_log("[screenshot] Starting screenshot utility...");

    /* Auto capture if run with --quick or -q */
    if (argc > 1 && (strcmp(argv[1], "-q") == 0 || strcmp(argv[1], "--quick") == 0)) {
        if (save_screenshot_ppm("/screenshot.ppm") == 0) {
            de_log("[screenshot] Quick capture saved to /screenshot.ppm");
            sys_exit(0);
        } else {
            de_log("[screenshot] Quick capture failed");
            sys_exit(1);
        }
    }

    /* Auto capture initial frame to /screenshot.ppm */
    save_screenshot_ppm("/screenshot.ppm");
    g_saved = 1;

    az_fb_info_t fb;
    unsigned int sw = 1280, sh = 800;
    if (az_fb_info(&fb) == 0 && fb.width > 0) {
        sw = fb.width;
        sh = fb.height;
    }

    int ret = uk_window_connect(&g_win, "Screenshot Utility",
                                (int)(sw / 2) - WIN_W / 2,
                                (int)(sh / 2) - WIN_H / 2,
                                WIN_W, WIN_H, MAP_ADDR, SERVER_CHAN);
    if (ret < 0) {
        de_log("[screenshot] FATAL: window connect failed");
        return -1;
    }

    draw_screenshot_ui();

    for (;;) {
        az_wm_msg_t msg;
        int r = az_channel_recv(g_win.client_chan, (az_ipc_msg_t *)&msg);
        if (r < 0) break;
        if (r != 0) continue;

        if (msg.type == AZ_WM_DESTROY_WINDOW) {
            break;
        }

        if (msg.type == AZ_WM_MOUSE_EVENT) {
            if (msg.mouse.buttons & AZ_MOUSE_BTN_LEFT) {
                int mx = msg.mouse.abs_x;
                int my = msg.mouse.abs_y;

                /* Save File button: (24, h-55, 140, 34) */
                if (mx >= 24 && mx <= 164 && my >= (int)g_win.height - 55 && my <= (int)g_win.height - 21) {
                    save_screenshot_ppm("/screenshot.ppm");
                    g_saved = 1;
                    draw_screenshot_ui();
                }
                /* Retake button: (180, h-55, 130, 34) */
                else if (mx >= 180 && mx <= 310 && my >= (int)g_win.height - 55 && my <= (int)g_win.height - 21) {
                    save_screenshot_ppm("/screenshot.ppm");
                    g_saved = 1;
                    draw_screenshot_ui();
                }
                /* Close button: (w-120, h-55, 96, 34) */
                else if (mx >= (int)g_win.width - 120 && mx <= (int)g_win.width - 24 &&
                         my >= (int)g_win.height - 55 && my <= (int)g_win.height - 21) {
                    break;
                }
            }
        }
    }

    az_wm_msg_t dmsg;
    memset(&dmsg, 0, sizeof(dmsg));
    dmsg.type = AZ_WM_DESTROY_WINDOW;
    dmsg.wid  = g_win.wid;
    az_channel_send(SERVER_CHAN, (az_ipc_msg_t *)&dmsg);

    sys_exit(0);
}
