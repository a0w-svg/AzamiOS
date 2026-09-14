/* ============================================================================
 * AzamiOS GUI Application Template (ui_kit)
 * ============================================================================ */
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include "../../libc/include/az/ipc.h"
#include "../azwm/protocol.h"
#include "../azwm/de_protocol.h"
#include "../azwm/de_font.h"
#include "../shared/ui_kit.h"

#define WIN_W       520
#define WIN_H       340
#define MAP_ADDR    ((void *)0x6F000000)
#define SERVER_CHAN 1

int main(int argc, char **argv)
{
    uk_window_t win;
    if (uk_window_connect(&win, "Azami GUI App", 140, 100, WIN_W, WIN_H, MAP_ADDR, SERVER_CHAN) < 0) {
        fprintf(stderr, "Failed to connect GUI window\n");
        return 1;
    }

    uk_fill_rect(&win, 0, 0, win.width, win.height, 0xFF1E1E2E);
    uk_draw_text(&win, 24, 40, "Welcome to AzamiOS GUI Development!", 0xFFCDD6F4);
    uk_draw_button(&win, 24, 80, 140, 32, "Click Me", UK_BTN_NORMAL);
    uk_invalidate(&win);

    bool running = true;
    while (running) {
        az_wm_msg_t msg;
        int r = az_channel_recv(win.client_chan, (az_ipc_msg_t *)&msg);
        if (r < 0) break;
        if (r != 0) continue;

        if (msg.type == AZ_WM_DESTROY_WINDOW) {
            running = false;
        }
    }

    return 0;
}
