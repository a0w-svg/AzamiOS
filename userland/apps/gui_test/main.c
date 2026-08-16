/* ============================================================================
 * AzamiOS — GUI Client Test Application
 * File: user/apps/gui_test/main.c
 * ============================================================================ */

#include "../../libc/include/az/ipc.h"
#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/string.h"
#include "../azwm/protocol.h"
#include <stdbool.h>

#define SERVER_CHAN 1

static int client_chan;
static unsigned int *window_pixels = 0;
static unsigned int win_w = 400;
static unsigned int win_h = 300;
static bool button_pressed = false;

/* ── UI Drawing Functions ───────────────────────────────────────────────── */
static void fill_rect(int rx, int ry, int rw, int rh, unsigned int color)
{
    if (!window_pixels) return;
    for (int y = ry; y < ry + rh; y++) {
        if (y < 0 || y >= (int)win_h) continue;
        for (int x = rx; x < rx + rw; x++) {
            if (x < 0 || x >= (int)win_w) continue;
            window_pixels[y * win_w + x] = color;
        }
    }
}

static void draw_ui(void)
{
    /* Background */
    fill_rect(0, 0, win_w, win_h, 0xFFEFF1F5);

    /* Button */
    unsigned int btn_color = button_pressed ? 0xFF8839EF : 0xFF1E66F5;
    fill_rect(100, 100, 200, 100, btn_color);
    
    /* (A real UI toolkit would render text here, but for now we just change colors) */
}

/* ── Entrypoint ──────────────────────────────────────────────────────────── */
int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    puts("[gui_test] Starting GUI client application...");


    client_chan = az_channel_create();
    if (client_chan < 0) {
        puts("[gui_test] Failed to create IPC channel.");
        return -1;
    }

    /* Send window creation request to compositor */
    az_wm_msg_t req;
    memset(&req, 0, sizeof(req));
    req.type = AZ_WM_CREATE_WINDOW;
    req.client_chan = client_chan;
    req.create.x = 200;
    req.create.y = 200;
    req.create.w = win_w;
    req.create.h = win_h;
    strcpy(req.create.title, "AzamiOS GUI Client");

    puts("[gui_test] Requesting window...");
    az_channel_send(SERVER_CHAN, (az_ipc_msg_t *)&req);

    /* Wait for creation confirmation */
    az_wm_msg_t resp;
    while (1) {
        if (az_channel_recv(client_chan, (az_ipc_msg_t *)&resp) == 0) {
            if (resp.type == AZ_WM_WINDOW_CREATED) {
                break;
            }
        }
    }

    puts("[gui_test] Window created! Mapping shared memory...");
    int shmem_id = resp.created.shmem_id;
    unsigned int wid = resp.created.assigned_wid;

    /* Map the pixel buffer. Pick a random user virtual address for demonstration. */
    void *map_addr = (void *)0x60000000;
    if (az_shmem_map(shmem_id, map_addr) < 0) {
        puts("[gui_test] Failed to map shared memory!");
        return -1;
    }
    window_pixels = (unsigned int *)map_addr;

    draw_ui();

    /* Send invalidate to trigger a redraw */
    az_wm_msg_t inv;
    memset(&inv, 0, sizeof(inv));
    inv.type = AZ_WM_INVALIDATE;
    inv.wid = wid;
    az_channel_send(SERVER_CHAN, (az_ipc_msg_t *)&inv);

    /* Event Loop */
    while (1) {
        az_wm_msg_t msg;
        int r = az_channel_recv(client_chan, (az_ipc_msg_t *)&msg);
        if (r < 0) {
            puts("[gui_test] IPC channel disconnected, exiting loop.");
            break;
        }

        bool needs_redraw = false;

        if (msg.type == AZ_WM_MOUSE_EVENT) {
            int mx = msg.mouse.abs_x;
            int my = msg.mouse.abs_y;
            bool lmb = (msg.mouse.buttons & AZ_MOUSE_BTN_LEFT);
            
            /* Simple hit test for button (100, 100, 200, 100) */
            bool in_button = (mx >= 100 && mx <= 300 && my >= 100 && my <= 200);

            if (in_button && lmb) {
                if (!button_pressed) {
                    button_pressed = true;
                    needs_redraw = true;
                }
            } else {
                if (button_pressed) {
                    button_pressed = false;
                    needs_redraw = true;
                }
            }
        }

        if (needs_redraw) {
            draw_ui();
            az_channel_send(SERVER_CHAN, (az_ipc_msg_t *)&inv); /* Send invalidate */
        }
    }

    sys_exit(0);
}
