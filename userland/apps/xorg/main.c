/* ============================================================================
 * AzamiOS — X.Org / X11 Display Server (Xorg / Xfbdev)
 * File: userland/apps/xorg/main.c
 *
 * Standalone POSIX X11 display server driving the hardware framebuffer (/dev/fb0)
 * and providing X11 protocol dispatch, window composition, and input routing.
 * ============================================================================ */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include "../../libc/include/az/ipc.h"
#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/Xproto.h>
#include "../azwm/protocol.h"

#define MAX_X_WINDOWS 64
#define DEFAULT_WIDTH  1280
#define DEFAULT_HEIGHT 800

typedef struct {
    unsigned int wid;
    unsigned int client_chan;
    int          x, y;
    unsigned int w, h;
    unsigned int shmem_id;
    uint32_t    *pixels;
    char         title[64];
    int          mapped;
    int          z_order;
} xorg_window_t;

typedef struct {
    uint32_t       *fb;
    uint32_t       *backbuffer;
    unsigned int    width;
    unsigned int    height;
    unsigned int    pitch;
    int             server_chan;
    xorg_window_t   windows[MAX_X_WINDOWS];
    int             num_windows;
    int             mouse_x;
    int             mouse_y;
    uint8_t         mouse_buttons;
    unsigned int    focused_wid;
} xorg_server_t;

static xorg_server_t g_xorg;

static void xorg_draw_cursor(uint32_t *buf, int cx, int cy, unsigned int pitch, unsigned int screen_h)
{
    /* Classic X11 arrow cursor (12x18) */
    static const uint16_t cursor_mask[18] = {
        0x8000, 0xC000, 0xE000, 0xF000, 0xF800, 0xFC00, 0xFE00, 0xFF00,
        0xFF80, 0xFFC0, 0xFFE0, 0xFE00, 0xEE00, 0xCE00, 0x8700, 0x0700,
        0x0380, 0x0380
    };
    static const uint16_t cursor_body[18] = {
        0x0000, 0x4000, 0x6000, 0x7000, 0x7800, 0x7C00, 0x7E00, 0x7F00,
        0x7F80, 0x7FC0, 0x7800, 0x4C00, 0x4C00, 0x0600, 0x0600, 0x0300,
        0x0300, 0x0000
    };

    int stride = pitch / 4;
    for (int r = 0; r < 18; r++) {
        int py = cy + r;
        if (py < 0 || py >= (int)screen_h) continue;
        for (int c = 0; c < 12; c++) {
            int px = cx + c;
            if (px < 0 || px >= (int)g_xorg.width) continue;
            if (cursor_mask[r] & (0x8000 >> c)) {
                if (cursor_body[r] & (0x8000 >> c)) {
                    buf[py * stride + px] = 0xFFFFFFFF; /* white inner */
                } else {
                    buf[py * stride + px] = 0xFF000000; /* black outline */
                }
            }
        }
    }
}

static void xorg_composite(void)
{
    if (!g_xorg.backbuffer || !g_xorg.fb) return;

    /* Render root desktop background (Catppuccin Mocha Base / X11 stippled weave) */
    int stride = g_xorg.pitch / 4;
    for (unsigned int y = 0; y < g_xorg.height; y++) {
        for (unsigned int x = 0; x < g_xorg.width; x++) {
            /* Classic X11 root weave pattern */
            uint32_t bg = ((x ^ y) & 4) ? 0xFF1E1E2E : 0xFF181825;
            g_xorg.backbuffer[y * stride + x] = bg;
        }
    }

    /* Composite mapped windows in Z-order */
    for (int i = 0; i < g_xorg.num_windows; i++) {
        xorg_window_t *w = &g_xorg.windows[i];
        if (!w->mapped || !w->pixels) continue;

        /* Window titlebar & frame decoration */
        int fx = w->x - 2;
        int fy = w->y - 24;
        int fw = (int)w->w + 4;
        int fh = (int)w->h + 26;

        /* Draw window border shadow */
        for (int dy = 0; dy < fh; dy++) {
            int py = fy + dy;
            if (py < 0 || py >= (int)g_xorg.height) continue;
            for (int dx = 0; dx < fw; dx++) {
                int px = fx + dx;
                if (px < 0 || px >= (int)g_xorg.width) continue;

                if (dy < 24) {
                    /* Titlebar: accented if focused */
                    uint32_t tcol = (w->wid == g_xorg.focused_wid) ? 0xFF313244 : 0xFF181825;
                    g_xorg.backbuffer[py * stride + px] = tcol;
                } else if (dx < 2 || dx >= fw - 2 || dy >= fh - 2) {
                    g_xorg.backbuffer[py * stride + px] = 0xFF45475A;
                }
            }
        }

        /* Draw window client pixels */
        for (unsigned int wy = 0; wy < w->h; wy++) {
            int py = w->y + (int)wy;
            if (py < 0 || py >= (int)g_xorg.height) continue;
            for (unsigned int wx = 0; wx < w->w; wx++) {
                int px = w->x + (int)wx;
                if (px < 0 || px >= (int)g_xorg.width) continue;
                g_xorg.backbuffer[py * stride + px] = w->pixels[wy * w->w + wx];
            }
        }
    }

    /* Composite mouse cursor */
    xorg_draw_cursor(g_xorg.backbuffer, g_xorg.mouse_x, g_xorg.mouse_y, g_xorg.pitch, g_xorg.height);

    /* Blit backbuffer to physical screen */
    memcpy(g_xorg.fb, g_xorg.backbuffer, (size_t)(g_xorg.pitch * g_xorg.height));
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("[Xorg] Initializing X11 Display Server v11.0 on AzamiOS...\n");

    /* Attempt to map hardware Linux framebuffer (/dev/fb0) */
    int fb_fd = open("/dev/fb0", O_RDWR);
    if (fb_fd >= 0) {
        struct fb_var_screeninfo var;
        struct fb_fix_screeninfo fix;
        if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &var) == 0 &&
            ioctl(fb_fd, FBIOGET_FSCREENINFO, &fix) == 0) {
            g_xorg.width = var.xres;
            g_xorg.height = var.yres;
            g_xorg.pitch = fix.line_length;
        } else {
            g_xorg.width = DEFAULT_WIDTH;
            g_xorg.height = DEFAULT_HEIGHT;
            g_xorg.pitch = DEFAULT_WIDTH * 4;
        }

        size_t fb_size = (size_t)(g_xorg.pitch * g_xorg.height);
        void *mapped = mmap(NULL, fb_size, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
        if (mapped != MAP_FAILED) {
            g_xorg.fb = (uint32_t *)mapped;
        } else {
            g_xorg.fb = (uint32_t *)malloc(fb_size);
        }
    } else {
        az_fb_info_t fb_info;
        memset(&fb_info, 0, sizeof(fb_info));
        if (az_fb_info(&fb_info) == 0 && fb_info.width > 0) {
            g_xorg.width = fb_info.width;
            g_xorg.height = fb_info.height;
            g_xorg.pitch = fb_info.pitch;
        } else {
            g_xorg.width = DEFAULT_WIDTH;
            g_xorg.height = DEFAULT_HEIGHT;
            g_xorg.pitch = DEFAULT_WIDTH * 4;
        }

        void *fb_vaddr = (void *)0x60000000UL;
        if (az_fb_map(fb_vaddr) >= 0) {
            g_xorg.fb = (uint32_t *)fb_vaddr;
        } else {
            g_xorg.fb = (uint32_t *)malloc((size_t)(g_xorg.pitch * g_xorg.height));
        }
    }

    g_xorg.backbuffer = (uint32_t *)malloc((size_t)(g_xorg.pitch * g_xorg.height));
    g_xorg.mouse_x = g_xorg.width / 2;
    g_xorg.mouse_y = g_xorg.height / 2;

    /* Create X11 Server IPC Channel */
    g_xorg.server_chan = az_channel_create();
    printf("[Xorg] X11 server listening on display :0 (channel %d, %ux%u)\n",
           g_xorg.server_chan, g_xorg.width, g_xorg.height);

    xorg_composite();

    /* Main X11 Request & Event Loop */
    for (;;) {
        az_wm_msg_t msg;
        memset(&msg, 0, sizeof(msg));
        if (az_channel_recv(g_xorg.server_chan, (az_ipc_msg_t *)&msg) < 0) {
            az_sleep(5);
            continue;
        }

        if (msg.type == AZ_WM_CREATE_WINDOW) {
            if (g_xorg.num_windows < MAX_X_WINDOWS) {
                unsigned int wid = (unsigned int)(g_xorg.num_windows + 1);
                xorg_window_t *w = &g_xorg.windows[g_xorg.num_windows++];
                w->wid = wid;
                w->client_chan = msg.client_chan;
                w->x = msg.create.x > 0 ? msg.create.x : 100 + (int)(wid * 30);
                w->y = msg.create.y > 0 ? msg.create.y : 100 + (int)(wid * 30);
                w->w = msg.create.w > 0 ? msg.create.w : 400;
                w->h = msg.create.h > 0 ? msg.create.h : 300;
                strncpy(w->title, msg.create.title, sizeof(w->title) - 1);
                w->mapped = 1;

                size_t bytes = (size_t)(w->w * w->h * 4);
                int pages = (int)((bytes + 4095) / 4096);
                w->shmem_id = (unsigned int)az_shmem_create(pages);

                void *map_addr = (void *)(0x50000000UL + ((unsigned long)w->shmem_id * 0x1000000UL));
                az_shmem_map((int)w->shmem_id, map_addr);
                w->pixels = (uint32_t *)map_addr;

                g_xorg.focused_wid = wid;

                /* Send AZ_WM_WINDOW_CREATED response */
                az_wm_msg_t resp;
                memset(&resp, 0, sizeof(resp));
                resp.type = AZ_WM_WINDOW_CREATED;
                resp.created.assigned_wid = wid;
                resp.created.shmem_id = w->shmem_id;
                resp.created.width = w->w;
                resp.created.height = w->h;
                az_channel_send((int)msg.client_chan, (az_ipc_msg_t *)&resp);

                xorg_composite();
            }
        } else if (msg.type == AZ_WM_DESTROY_WINDOW) {
            for (int i = 0; i < g_xorg.num_windows; i++) {
                if (g_xorg.windows[i].wid == msg.wid) {
                    if (g_xorg.windows[i].pixels) {
                        az_shmem_unmap((int)g_xorg.windows[i].shmem_id, g_xorg.windows[i].pixels);
                    }
                    /* Remove from list */
                    for (int j = i; j < g_xorg.num_windows - 1; j++) {
                        g_xorg.windows[j] = g_xorg.windows[j + 1];
                    }
                    g_xorg.num_windows--;
                    break;
                }
            }
            xorg_composite();
        } else if (msg.type == AZ_WM_INVALIDATE) {
            xorg_composite();
        } else if (msg.type == AZ_WM_MOVE_WINDOW) {
            for (int i = 0; i < g_xorg.num_windows; i++) {
                if (g_xorg.windows[i].wid == msg.wid) {
                    g_xorg.windows[i].x = msg.move.x;
                    g_xorg.windows[i].y = msg.move.y;
                    break;
                }
            }
            xorg_composite();
        }
    }

    return 0;
}
