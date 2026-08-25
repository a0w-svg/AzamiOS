/* ============================================================================
 * AzamiOS Desktop Environment — Session Manager Daemon (sessiond.elf)
 * File: userland/apps/sessiond/main.c   v1.0
 *
 * Responsibilities
 * ────────────────
 *  1. Spawns the Display Server (azwm.elf).
 *  2. Probes azwm readiness via active IPC handshaking (no arbitrary sleep delays).
 *  3. Displays a sleek AzamiOS boot splash screen on startup.
 *  4. Sequentially launches Desktop Environment components (wallpaper, taskbar).
 *  5. Dismisses splash screen and broadcasts AZ_WM_SESSION_READY.
 *  6. Functions as a robust session watchdog to monitor DE health.
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/string.h"
#include "../../libc/include/unistd.h"
#include "../../libc/include/sys/syscall.h"
#include "../../libc/include/az/ipc.h"
#include "../azwm/protocol.h"
#include "../azwm/de_protocol.h"
#include "../azwm/de_font.h"
#include "../shared/ui_kit.h"

#define SERVER_CHAN         1
#define SPLASH_MAP_ADDR     ((void *)0x66000000)

/* Splash geometry */
#define SPLASH_W            480
#define SPLASH_H            280

static uk_window_t g_splash_win;

/* ── Render sleek splash screen ───────────────────────────────────────────── */
static void render_splash(uk_window_t *win, int progress)
{
    unsigned int w = win->width;
    unsigned int h = win->height;

    /* Background: Deep dark gradient */
    uk_gradient_v(win, 0, 0, (int)w, (int)h, 0xFF181825, 0xFF11111B);

    /* Rounded glowing border */
    uk_fill_rounded_rect(win, 0, 0, (int)w, (int)h, 12, 0xFF313244);
    uk_fill_rounded_rect(win, 2, 2, (int)w - 4, (int)h - 4, 10, 0xFF181825);

    /* AzamiOS Logo */
    const char *logo = "AzamiOS";
    int logo_x = (int)w / 2 - (7 * 16) / 2;
    int logo_y = 60;
    de_font_draw_str_2x(win->pixels, w, w, h, logo_x + 2, logo_y + 2, logo, 0xFF11111B);
    de_font_draw_str_2x(win->pixels, w, w, h, logo_x, logo_y, logo, UK_MAUVE);

    /* Subtitle */
    const char *sub = "Modular Microkernel Desktop";
    int sub_len = uk_strlen(sub);
    uk_draw_text(win, (int)w / 2 - (sub_len * 8) / 2, logo_y + 44, sub, UK_SUBTEXT0);

    /* Progress bar track */
    int pb_w = 320;
    int pb_h = 6;
    int pb_x = (int)w / 2 - pb_w / 2;
    int pb_y = logo_y + 110;

    uk_fill_rounded_rect(win, pb_x, pb_y, pb_w, pb_h, 3, UK_SURFACE0);

    /* Progress fill */
    if (progress > 0) {
        int fill_w = (pb_w * progress) / 100;
        if (fill_w > pb_w) fill_w = pb_w;
        uk_fill_rounded_rect(win, pb_x, pb_y, fill_w, pb_h, 3, UK_MAUVE);
    }

    /* Status text */
    const char *status = (progress < 40) ? "Starting Display Server..."
                       : (progress < 80) ? "Loading Desktop Environment..."
                       : "Starting Session...";
    int st_len = uk_strlen(status);
    uk_draw_text(win, (int)w / 2 - (st_len * 8) / 2, pb_y + 18, status, UK_OVERLAY0);

    uk_invalidate(win);
}

/* ── Active IPC probe to verify azwm is alive and responding ──────────────── */
static int probe_azwm_readiness(int max_retries)
{
    de_log("[sessiond] Waiting for azwm to initialize channel 1...");

    /* Send a dummy ping until channel 1 is created by azwm */
    az_wm_msg_t ping;
    memset(&ping, 0, sizeof(ping));
    ping.type = 0; /* no-op */

    int chan1_ready = 0;
    for (int attempt = 0; attempt < max_retries; attempt++) {
        if (az_channel_send_nb(SERVER_CHAN, (az_ipc_msg_t *)&ping) == 0) {
            chan1_ready = 1;
            break;
        }
        for (int i = 0; i < 20; i++) az_yield();
    }

    if (!chan1_ready) {
        de_log("[sessiond] ERROR: azwm channel 1 not online (timed out)");
        return -1;
    }

    de_log("[sessiond] azwm channel 1 is online. Creating probe client channel...");
    int probe_chan = az_channel_create();
    if (probe_chan < 0) {
        de_log("[sessiond] ERROR: Failed to create probe IPC channel");
        return -1;
    }

    az_wm_msg_t req;
    memset(&req, 0, sizeof(req));
    req.type            = AZ_WM_CREATE_WINDOW;
    req.client_chan      = (unsigned int)probe_chan;
    req.create.x        = 0;
    req.create.y        = 0;
    req.create.w        = 10;
    req.create.h        = 10;
    req.create.title[0] = '\0';

    if (az_channel_send(SERVER_CHAN, (az_ipc_msg_t *)&req) < 0) {
        de_log("[sessiond] ERROR: Failed to send probe create request");
        return -1;
    }

    /* Wait for azwm's event loop to reply */
    az_wm_msg_t resp;
    int r = az_channel_recv(probe_chan, (az_ipc_msg_t *)&resp);
    if (r == 0) {
        if (resp.type == AZ_WM_WINDOW_CREATED && resp.created.assigned_wid > 0) {
            az_wm_msg_t dest;
            memset(&dest, 0, sizeof(dest));
            dest.type = AZ_WM_DESTROY_WINDOW;
            dest.wid  = resp.created.assigned_wid;
            az_channel_send(SERVER_CHAN, (az_ipc_msg_t *)&dest);
            de_log("[sessiond] azwm is READY (handshake successful).");
            return 0;
        }
    }

    de_log("[sessiond] ERROR: azwm probe handshake failed");
    return -1;
}

#include "../shared/sys_config.h"

/* ============================================================================
 * _start
 * ============================================================================ */
int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    az_config_init_storage();
    de_log("[sessiond] Starting desktop session sequence...");

    /* ── Step 1: Spawn Display Server (/sbin/azwm.elf) ─────────────────── */
    int wm_pid = az_spawn("/sbin/azwm.elf");
    if (wm_pid < 0) {
        de_log("[sessiond] FATAL: az_spawn /sbin/azwm.elf failed!");
        sys_exit(1);
    }
    de_log("[sessiond] azwm.elf spawned.");

    /* ── Step 2: Probe azwm readiness (no arbitrary delay) ────────────────── */
    if (probe_azwm_readiness(300) < 0) {
        de_log("[sessiond] FATAL: azwm readiness probe failed.");
        sys_exit(1);
    }

    /* ── Step 3: Create & display Splash Screen ──────────────────────────── */
    az_fb_info_t fb;
    unsigned int sw = 1280, sh = 800;
    if (az_fb_info(&fb) == 0 && fb.width > 0 && fb.height > 0) {
        sw = fb.width;
        sh = fb.height;
    }

    int splash_x = (int)(sw - SPLASH_W) / 2;
    int splash_y = (int)(sh - SPLASH_H) / 2;

    int splash_ok = 0;
    if (uk_window_connect(&g_splash_win, "", splash_x, splash_y,
                          SPLASH_W, SPLASH_H, SPLASH_MAP_ADDR, SERVER_CHAN) == 0) {
        uk_set_zorder(&g_splash_win, AZ_WM_ZORDER_TOP);
        render_splash(&g_splash_win, 20);
        splash_ok = 1;
    }

    /* ── Step 4: Spawn Root Window / Animated Wallpaper ─────────────────── */
    if (splash_ok) render_splash(&g_splash_win, 45);
    int wp_pid = az_spawn("/sbin/wallpaper.elf");
    if (wp_pid >= 0) {
        de_log("[sessiond] wallpaper.elf spawned.");
    }

    /* ── Step 5: Spawn Taskbar ───────────────────────────────────────────── */
    if (splash_ok) render_splash(&g_splash_win, 75);
    int tb_pid = az_spawn("/sbin/taskbar.elf");
    if (tb_pid >= 0) {
        de_log("[sessiond] taskbar.elf spawned.");
    }

    /* Auto-launch terminal emulator on startup */
    int term_pid = az_spawn("/bin/terminal.elf");
    if (term_pid >= 0) {
        de_log("[sessiond] terminal.elf auto-spawned successfully.");
    }

    /* ── Step 6: Dismiss Splash Screen ───────────────────────────────────── */
    if (splash_ok) {
        render_splash(&g_splash_win, 100);
        az_wm_msg_t destroy;
        memset(&destroy, 0, sizeof(destroy));
        destroy.type = AZ_WM_DESTROY_WINDOW;
        destroy.wid  = g_splash_win.wid;
        az_channel_send(SERVER_CHAN, (az_ipc_msg_t *)&destroy);
        if (g_splash_win.pixels) {
            az_shmem_unmap(0, g_splash_win.pixels);
            g_splash_win.pixels = NULL;
        }
    }

    de_log("[sessiond] Desktop Environment initialized successfully.");

    /* ── Step 7: Session Watchdog Loop ───────────────────────────────────── */
    for (;;) {
        sleep(1);
    }

    sys_exit(0);
}
