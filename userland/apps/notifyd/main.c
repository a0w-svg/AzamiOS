/* ============================================================================
 * AzamiOS — notifyd: Desktop Notification Daemon
 * File: userland/apps/notifyd/main.c
 *
 * Role
 * ────
 * notifyd subscribes to the azwm event bus and listens for AZ_WM_NOTIFY
 * broadcasts.  When one arrives it moves its top-z-order overlay window on screen
 * and renders a toast using uk_draw_toast() for ~3 seconds, then moves it
 * off-screen.
 * ============================================================================ */

#include "../shared/ui_kit.h"
#include "../shared/de_log.h"
#include "../../libc/include/string.h"
#include "../../libc/include/stdio.h"
#include "../../libc/include/unistd.h"

/* Approx frames at ~60 fps * 3 s */
#define TOAST_FRAMES     180
/* Fade-out starts at this frame (last 60 frames = 1 s) */
#define TOAST_FADE_START 120

/* Screen dimensions fallback */
#define SCR_W_DEFAULT 1280
#define SCR_H_DEFAULT  800

/* Overlay window geometry */
#define TOAST_OVL_W   300
#define TOAST_OVL_H    80
#define TOAST_MARGIN    8

#define NOTIFYD_MAP   ((void *)0x6F000000)
#define SERVER_CHAN   1


int main(void)
{
    puts("[notifyd] AzamiOS notification daemon starting");

    int screen_w = SCR_W_DEFAULT;
    int screen_h = SCR_H_DEFAULT;
    az_fb_info_t fb_info;
    if (az_fb_info(&fb_info) == 0 && fb_info.width > 0 && fb_info.height > 0) {
        screen_w = (int)fb_info.width;
        screen_h = (int)fb_info.height;
    }
    (void)screen_h;

    int ox = screen_w - TOAST_OVL_W - TOAST_MARGIN;
    int oy = TOAST_MARGIN + 52; /* below taskbar */

    uk_window_t overlay;
    /* Create overlay initially offscreen with empty title (no decorations/titlebar) */
    if (uk_window_connect(&overlay, "", -1000, -1000, TOAST_OVL_W, TOAST_OVL_H, NOTIFYD_MAP, SERVER_CHAN) != 0) {
        puts("[notifyd] ERROR: uk_window_connect failed");
        return 1;
    }

    uk_set_zorder(&overlay, AZ_WM_ZORDER_TOP);
    uk_subscribe_events(&overlay);
    uk_clear(&overlay, 0x00000000);
    uk_invalidate(&overlay);

    puts("[notifyd] Subscribed to compositor events; listening for notifications...");

    char toast_title[AZ_WM_NOTIFY_TITLE_MAX];
    char toast_body[AZ_WM_NOTIFY_BODY_MAX];
    memset(toast_title, 0, sizeof(toast_title));
    memset(toast_body, 0, sizeof(toast_body));
    int  toast_frames_left = 0;
    int  toast_active      = 0;

    for (;;) {
        az_wm_msg_t msg;
        int got = az_channel_recv_nb(overlay.client_chan, (az_ipc_msg_t *)&msg);
        if (got == 0) {
            if (msg.type == AZ_WM_NOTIFY) {
                az_wm_notify_payload_t *pl = AZ_WM_MSG_NOTIFY(&msg);
                pl->title[AZ_WM_NOTIFY_TITLE_MAX - 1] = '\0';
                pl->body[AZ_WM_NOTIFY_BODY_MAX - 1]   = '\0';
                memcpy(toast_title, pl->title, AZ_WM_NOTIFY_TITLE_MAX);
                memcpy(toast_body,  pl->body,  AZ_WM_NOTIFY_BODY_MAX);

                de_log_fmt("[notifyd] NOTIFY: ", toast_title);

                toast_frames_left = TOAST_FRAMES;
                if (!toast_active) {
                    toast_active = 1;
                    uk_move_window(&overlay, ox, oy);
                }
            }
        }

        if (toast_active) {
            unsigned int alpha = 255;
            if (toast_frames_left < (TOAST_FRAMES - TOAST_FADE_START)) {
                alpha = (unsigned int)(
                    255u * (unsigned int)toast_frames_left /
                    (unsigned int)(TOAST_FRAMES - TOAST_FADE_START));
            }

            uk_clear(&overlay, 0x00000000);
            uk_draw_toast(&overlay, toast_title, toast_body, alpha);
            uk_invalidate(&overlay);

            toast_frames_left--;
            if (toast_frames_left <= 0) {
                toast_active = 0;
                uk_clear(&overlay, 0x00000000);
                uk_invalidate(&overlay);
                uk_move_window(&overlay, -1000, -1000);
            }
            usleep(16000); /* ~60 FPS during toast animation */
        } else {
            usleep(50000); /* Idle: yield CPU, poll every 50ms */
        }
    }

    return 0;
}
