/* ============================================================================
 * AzamiOS — Display Server (azwm) Main Event Loop   v2.0
 * File: userland/apps/azwm/main.c
 *
 * Changes from v1.0
 * -----------------
 *  • Integrated de_compositor.c: de_comp_init(), de_comp_handle_message(),
 *    de_comp_broadcast_created/destroyed/focus(), de_comp_enforce_zorder()
 *  • Mouse event handling: accumulate abs coords from relative deltas,
 *    hit-test windows, forward to focused client via AZ_WM_MOUSE_EVENT
 *  • Focus change: left-click on window title bar → compositor_focus_window()
 *    + de_comp_broadcast_focus()
 *  • Close button: left-click on close box → compositor_destroy_window()
 *    + de_comp_broadcast_destroyed()
 *  • Input polling: az_input_poll() for keyboard and mouse events,
 *    forwarded to focused window as AZ_WM_KEY_EVENT / AZ_WM_MOUSE_EVENT
 * ============================================================================ */

#include "compositor.h"
#include "de_compositor.h"
#include "../shared/de_log.h"
#include "../../libc/include/az/ipc.h"
#include "../../libc/include/stdio.h"
#include "../../libc/include/string.h"
#include "../../libc/include/unistd.h"
#include <stdbool.h>

/* Display geometry is resolved at runtime via az_fb_info() — these are
 * safe fallbacks used only if the syscall fails.                        */
#define DISPLAY_WIDTH_DEFAULT   1280
#define DISPLAY_HEIGHT_DEFAULT   800
#define AZWM_TASKBAR_H           52

/* ── Framebuffer mapping ────────────────────────────────────────────────────── */
/*
 * map_shared_memory — Map the hardware framebuffer and allocate a back buffer.
 *
 * @frontbuf  [in/out] VA at which to map the HW framebuffer
 * @backbuf   [in/out] VA at which to map the back buffer (shared memory)
 * @screen_w  actual screen width  in pixels (from az_fb_info)
 * @screen_h  actual screen height in pixels
 *
 * Returns 0 on success, -1 if the front buffer could not be mapped (fatal).
 */
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>

static int map_shared_memory(void **frontbuf, void **backbuf,
                              unsigned int screen_w, unsigned int screen_h)
{
    /* 1. Map physical hardware VRAM into frontbuf */
    int mapped_front = 0;
    int fb_fd = open("/dev/fb0", O_RDWR);
    if (fb_fd >= 0) {
        struct fb_var_screeninfo var;
        struct fb_fix_screeninfo fix;
        if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &var) == 0 &&
            ioctl(fb_fd, FBIOGET_FSCREENINFO, &fix) == 0) {
            size_t total_vram = (size_t)(fix.line_length * var.yres_virtual);
            if (total_vram == 0) total_vram = (size_t)screen_w * screen_h * 4 * 2;
            void *mapped = mmap(*frontbuf, total_vram, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
            if (mapped != MAP_FAILED) {
                *frontbuf = mapped;
                mapped_front = 1;
            }
        }
        close(fb_fd);
    }

    if (!mapped_front) {
        /* Direct microkernel framebuffer mapping */
        int fb_ret = az_fb_map(*frontbuf);
        if (fb_ret < 0) {
            puts("[azwm] FATAL: az_fb_map failed — cannot reach display hardware!");
            return -1;
        }
        mapped_front = 1;
    }

    /* 2. Allocate an offscreen back buffer sized to the screen (+ 1 spare page) */
    unsigned long buf_bytes  = (unsigned long)screen_w * screen_h * 4;
    int           page_count = (int)((buf_bytes + 4095UL) / 4096UL) + 1;

    int shmem_id = az_shmem_create(page_count);
    if (shmem_id >= 0) {
        int sm_ret = az_shmem_map(shmem_id, *backbuf);
        if (sm_ret < 0) {
            puts("[azwm] WARNING: az_shmem_map backbuf failed — using front buffer as back buffer");
            *backbuf = *frontbuf;
        }
    } else {
        puts("[azwm] WARNING: az_shmem_create failed — using front buffer as back buffer");
        *backbuf = *frontbuf;
    }

    puts("[azwm] Hardware Framebuffer & Shared Memory Compositor initialized.");
    return 0;
}

/* ── Close-button hit test ──────────────────────────────────────────────────── */
/*
 * The close button is rendered by compositor.c at:
 *   x: win->x + win->width - 20   (right edge, 20px from right)
 *   y: win->y - AZWM_TITLEBAR_H + 4
 *   size: 16×16 px
 * We add a 2px margin to make it easier to click.
 */
static bool hit_close_button(az_window_t *win, int mx, int my)
{
    bool has_frame = (win->title[0] != '\0');
    if (!has_frame) return false;
    int bx = win->x + (int)win->width - 18;
    int by = win->y - AZWM_TITLEBAR_H + 12;
    return ((mx - bx) * (mx - bx) + (my - by) * (my - by) <= 9 * 9);
}

/* ── Minimise-button hit test ───────────────────────────────────────────────── */
static bool hit_minimize_button(az_window_t *win, int mx, int my)
{
    bool has_frame = (win->title[0] != '\0');
    if (!has_frame) return false;
    int bx = win->x + (int)win->width - 38;
    int by = win->y - AZWM_TITLEBAR_H + 12;
    return ((mx - bx) * (mx - bx) + (my - by) * (my - by) <= 9 * 9);
}

/* ── Maximise-button hit test ───────────────────────────────────────────────── */
static bool hit_maximize_button(az_window_t *win, int mx, int my)
{
    bool has_frame = (win->title[0] != '\0');
    if (!has_frame) return false;
    int bx = win->x + (int)win->width - 58;
    int by = win->y - AZWM_TITLEBAR_H + 12;
    return ((mx - bx) * (mx - bx) + (my - by) * (my - by) <= 9 * 9);
}

/* ── Title bar hit test (for drag / focus on click) ─────────────────────────── */
static bool hit_titlebar(az_window_t *win, int mx, int my)
{
    bool has_frame = (win->title[0] != '\0');
    if (!has_frame) return false;
    int bx = win->x - AZWM_BORDER_W;
    int by = win->y - AZWM_TITLEBAR_H - AZWM_BORDER_W;
    int bw = (int)win->width + 2 * AZWM_BORDER_W;
    return (mx >= bx && mx < bx + bw && my >= by && my < by + AZWM_TITLEBAR_H + AZWM_BORDER_W);
}

/* ── Window body hit test ───────────────────────────────────────────────────── */
static bool hit_window(az_window_t *win, int mx, int my)
{
    bool has_frame = (win->title[0] != '\0');
    int bx = win->x;
    int by = win->y;
    int bw = (int)win->width;
    int bh = (int)win->height;
    
    if (has_frame) {
        bx -= AZWM_BORDER_W;
        by -= (AZWM_TITLEBAR_H + AZWM_BORDER_W);
        bw += 2 * AZWM_BORDER_W;
        bh += (AZWM_TITLEBAR_H + 2 * AZWM_BORDER_W);
    }
    
    return (mx >= bx && mx < bx + bw && my >= by && my < by + bh);
}

/* ── Window resize grip hit test (bottom-right corner) ──────────────────────── */
static bool hit_resize_grip(az_window_t *win, int mx, int my)
{
    bool has_frame = (win->title[0] != '\0');
    if (!has_frame || win->maximized) return false;
    int rx = win->x + (int)win->width;
    int ry = win->y + (int)win->height;
    return (mx >= rx - 14 && mx <= rx + AZWM_BORDER_W + 2 &&
            my >= ry - 14 && my <= ry + AZWM_BORDER_W + 2);
}

/* ── Find topmost window at screen coords ───────────────────────────────────── */
static az_window_t *find_window_at(az_compositor_t *comp, int mx, int my)
{
    az_window_t *curr = comp->list_head;
    while (curr) {
        if (curr->wid != 0 && curr->visible) {
            if (hit_window(curr, mx, my))
                return curr;
        }
        curr = curr->next;
    }
    return (az_window_t *)0;
}

/* ============================================================================
 * _start
 * ============================================================================ */
int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    de_log("[azwm] main entered");


    /* ── Create server IPC channel ──────────────────────────────────────── */
    int server_chan = az_channel_create();
    if (server_chan < 0) {
        de_log("[azwm] FATAL: az_channel_create failed");
        return -1;
    }
    de_log("[azwm] IPC channel created");

    /* ── Query actual framebuffer geometry from the kernel ──────────────── */
    az_fb_info_t fb_info;
    unsigned int screen_w     = DISPLAY_WIDTH_DEFAULT;
    unsigned int screen_h     = DISPLAY_HEIGHT_DEFAULT;
    unsigned int screen_pitch = DISPLAY_WIDTH_DEFAULT * 4;

    if (az_fb_info(&fb_info) == 0 && fb_info.width > 0 && fb_info.height > 0) {
        screen_w     = fb_info.width;
        screen_h     = fb_info.height;
        screen_pitch = fb_info.pitch;
        de_log("[azwm] Framebuffer geometry obtained from kernel");
    } else {
        de_log("[azwm] WARNING: az_fb_info failed — using default 1280x800");
    }

    /* ── Map framebuffer ────────────────────────────────────────────────── */
    unsigned int *frontbuf = (unsigned int *)0x40000000;
    unsigned int *backbuf  = (unsigned int *)0x42000000; /* Ensure enough gap for any resolution */
    if (map_shared_memory((void **)&frontbuf, (void **)&backbuf,
                           screen_w, screen_h) < 0) {
        de_log("[azwm] FATAL: Could not map framebuffer — exiting");
        return -1;
    }
    de_log("[azwm] Framebuffer mapped");

    de_log("[azwm] Starting Display Server v2.0...");

    /* ── Compositor init ────────────────────────────────────────────────── */
    az_compositor_t comp;
    compositor_init(&comp, frontbuf, backbuf,
                    screen_w, screen_h, screen_pitch, server_chan);
    de_log("[azwm] Compositor initialised");

    /* ── DE compositor extension init ──────────────────────────────────── */
    de_comp_state_t de_state;
    de_comp_init(&de_state);
    de_log("[azwm] DE compositor extension initialised.");

    /* NOTE: input is handled inline in the main event loop below via az_input_poll(). */

    /* ── Initial render ─────────────────────────────────────────────────── */
    de_log("[azwm] Drawing initial background...");
    compose_screen(&comp);

    de_log("[azwm] Entering main event loop...");

    /* ── Absolute mouse position (accumulated from deltas) ──────────────── */
    int abs_x = (int)(screen_w / 2);
    int abs_y = (int)(screen_h / 2);

    /* ── Track previous focused wid for broadcast ───────────────────────── */
    unsigned int prev_focus_wid = 0;

    /* ── Track mouse state for dragging and resizing ─────────────────────── */
    unsigned int prev_buttons = 0;
    unsigned int drag_wid = 0;
    int drag_off_x = 0;
    int drag_off_y = 0;

    unsigned int resize_wid = 0;
    int resize_start_w = 0, resize_start_h = 0;
    int resize_start_mx = 0, resize_start_my = 0;

    unsigned int last_click_time = 0;
    int last_click_x = 0, last_click_y = 0;

    bool running = true;
    while (running) {
        bool redraw_needed = false;
        bool cursor_moved = false;

        /* ── 1. Poll for hardware input events (non-blocking) ──────────── */
        az_input_event_t ev;
        while (az_input_poll(&ev) == 0) {
            if (ev.type == AZ_INPUT_EVENT_MOUSE) {
                /* Smooth acceleration curve */
                int mdx = (int)ev.mouse_dx;
                int mdy = (int)ev.mouse_dy;
                int speed_sq = mdx * mdx + mdy * mdy;
                if (speed_sq > 36) {
                    mdx = (mdx * 14) / 10;
                    mdy = (mdy * 14) / 10;
                }

                abs_x += mdx;
                abs_y += mdy;
                if (abs_x < 0) abs_x = 0;
                if (abs_x >= (int)screen_w)  abs_x = (int)screen_w  - 1;
                if (abs_y < 0) abs_y = 0;
                if (abs_y >= (int)screen_h) abs_y = (int)screen_h - 1;

                comp.cursor_x = abs_x;
                comp.cursor_y = abs_y;
                cursor_moved = true;

                bool lclick_now = (ev.mouse_buttons & AZ_MOUSE_BTN_LEFT) != 0;
                bool lclick = lclick_now && !(prev_buttons & AZ_MOUSE_BTN_LEFT);
                bool rclick_now = (ev.mouse_buttons & AZ_MOUSE_BTN_RIGHT) != 0;
                bool rclick = rclick_now && !(prev_buttons & AZ_MOUSE_BTN_RIGHT);
                bool btn_press = lclick || rclick;
                bool lrelease = !lclick_now && (prev_buttons & AZ_MOUSE_BTN_LEFT);
                prev_buttons = ev.mouse_buttons;

                if (lrelease) {
                    if (drag_wid != 0) {
                        az_window_t *dwin = (az_window_t *)0;
                        for (int i = 0; i < AZWM_MAX_WINDOWS; i++) {
                            if (comp.window_pool[i].wid == drag_wid) {
                                dwin = &comp.window_pool[i];
                                break;
                            }
                        }
                        if (dwin && comp.snap_preview_mode != 0) {
                            int mode = comp.snap_preview_mode;
                            dwin->saved_x = dwin->x; dwin->saved_y = dwin->y;
                            dwin->saved_w = dwin->width; dwin->saved_h = dwin->height;

                            if (mode == 1) {
                                /* Snap Left Half */
                                dwin->maximized = 0;
                                dwin->x = AZWM_BORDER_W;
                                dwin->y = AZWM_TITLEBAR_H + AZWM_BORDER_W;
                                dwin->width = (screen_w / 2) - 2 * AZWM_BORDER_W;
                                dwin->height = screen_h - AZWM_TASKBAR_H - AZWM_TITLEBAR_H - 2 * AZWM_BORDER_W;
                            } else if (mode == 2) {
                                /* Snap Right Half */
                                dwin->maximized = 0;
                                dwin->x = (screen_w / 2) + AZWM_BORDER_W;
                                dwin->y = AZWM_TITLEBAR_H + AZWM_BORDER_W;
                                dwin->width = (screen_w / 2) - 2 * AZWM_BORDER_W;
                                dwin->height = screen_h - AZWM_TASKBAR_H - AZWM_TITLEBAR_H - 2 * AZWM_BORDER_W;
                            } else if (mode == 3) {
                                /* Snap Maximize */
                                dwin->maximized = 1;
                                dwin->x = AZWM_BORDER_W;
                                dwin->y = AZWM_TITLEBAR_H + AZWM_BORDER_W;
                                dwin->width = screen_w - 2 * AZWM_BORDER_W;
                                dwin->height = screen_h - AZWM_TASKBAR_H - AZWM_TITLEBAR_H - 2 * AZWM_BORDER_W;
                            } else if (mode == 4) {
                                /* Snap Top-Left Quarter */
                                dwin->maximized = 0;
                                dwin->x = AZWM_BORDER_W;
                                dwin->y = AZWM_TITLEBAR_H + AZWM_BORDER_W;
                                dwin->width = (screen_w / 2) - 2 * AZWM_BORDER_W;
                                dwin->height = (screen_h - AZWM_TASKBAR_H) / 2 - AZWM_TITLEBAR_H - 2 * AZWM_BORDER_W;
                            } else if (mode == 5) {
                                /* Snap Top-Right Quarter */
                                dwin->maximized = 0;
                                dwin->x = (screen_w / 2) + AZWM_BORDER_W;
                                dwin->y = AZWM_TITLEBAR_H + AZWM_BORDER_W;
                                dwin->width = (screen_w / 2) - 2 * AZWM_BORDER_W;
                                dwin->height = (screen_h - AZWM_TASKBAR_H) / 2 - AZWM_TITLEBAR_H - 2 * AZWM_BORDER_W;
                            } else if (mode == 6) {
                                /* Snap Bottom-Left Quarter */
                                dwin->maximized = 0;
                                dwin->x = AZWM_BORDER_W;
                                dwin->y = (screen_h - AZWM_TASKBAR_H) / 2 + AZWM_TITLEBAR_H + AZWM_BORDER_W;
                                dwin->width = (screen_w / 2) - 2 * AZWM_BORDER_W;
                                dwin->height = (screen_h - AZWM_TASKBAR_H) / 2 - AZWM_TITLEBAR_H - 2 * AZWM_BORDER_W;
                            } else if (mode == 7) {
                                /* Snap Bottom-Right Quarter */
                                dwin->maximized = 0;
                                dwin->x = (screen_w / 2) + AZWM_BORDER_W;
                                dwin->y = (screen_h - AZWM_TASKBAR_H) / 2 + AZWM_TITLEBAR_H + AZWM_BORDER_W;
                                dwin->width = (screen_w / 2) - 2 * AZWM_BORDER_W;
                                dwin->height = (screen_h - AZWM_TASKBAR_H) / 2 - AZWM_TITLEBAR_H - 2 * AZWM_BORDER_W;
                            }
                            redraw_needed = true;
                        }
                    }
                    comp.snap_preview_mode = 0;
                    drag_wid = 0;
                    resize_wid = 0;
                }

                if (resize_wid != 0) {
                    /* We are actively resizing a window */
                    az_window_t *rwin = (az_window_t *)0;
                    for (int i = 0; i < AZWM_MAX_WINDOWS; i++) {
                        if (comp.window_pool[i].wid == resize_wid) {
                            rwin = &comp.window_pool[i];
                            break;
                        }
                    }
                    if (rwin) {
                        int nw = resize_start_w + (abs_x - resize_start_mx);
                        int nh = resize_start_h + (abs_y - resize_start_my);
                        if (nw < 160) nw = 160;
                        if (nh < 80)  nh = 80;
                        if (nw > (int)screen_w) nw = (int)screen_w;
                        if (nh > (int)screen_h - AZWM_TASKBAR_H) nh = (int)screen_h - AZWM_TASKBAR_H;
                        rwin->width  = (unsigned int)nw;
                        rwin->height = (unsigned int)nh;
                        redraw_needed = true;
                    } else {
                        resize_wid = 0;
                    }
                } else if (drag_wid != 0) {
                    /* We are currently dragging a window */
                    az_window_t *dwin = (az_window_t *)0;
                    for (int i = 0; i < AZWM_MAX_WINDOWS; i++) {
                        if (comp.window_pool[i].wid == drag_wid) {
                            dwin = &comp.window_pool[i];
                            break;
                        }
                    }
                    if (dwin) {
                        int old_x = dwin->x;
                        int old_y = dwin->y;
                        int new_x = abs_x - drag_off_x;
                        int new_y = abs_y - drag_off_y;

                        if (old_x != new_x || old_y != new_y) {
                            int margin = 16;
                            compositor_damage(&comp, old_x - AZWM_BORDER_W - margin, old_y - AZWM_TITLEBAR_H - AZWM_BORDER_W - margin,
                                              (int)dwin->width + 2 * AZWM_BORDER_W + 2 * margin, (int)dwin->height + AZWM_TITLEBAR_H + 2 * AZWM_BORDER_W + 2 * margin);
                            dwin->x = new_x;
                            dwin->y = new_y;
                            compositor_damage(&comp, new_x - AZWM_BORDER_W - margin, new_y - AZWM_TITLEBAR_H - AZWM_BORDER_W - margin,
                                              (int)dwin->width + 2 * AZWM_BORDER_W + 2 * margin, (int)dwin->height + AZWM_TITLEBAR_H + 2 * AZWM_BORDER_W + 2 * margin);
                        }

                        /* Dynamic 6-Way Aero Snap Guide Preview while dragging */
                        int prev_mode = comp.snap_preview_mode;
                        if (abs_y <= 12) {
                            if (abs_x <= 32) comp.snap_preview_mode = 4; /* Top-Left 1/4 */
                            else if (abs_x >= (int)screen_w - 33) comp.snap_preview_mode = 5; /* Top-Right 1/4 */
                            else comp.snap_preview_mode = 3; /* Maximize */
                        } else if (abs_y >= (int)screen_h - 52) {
                            if (abs_x <= 32) comp.snap_preview_mode = 6; /* Bottom-Left 1/4 */
                            else if (abs_x >= (int)screen_w - 33) comp.snap_preview_mode = 7; /* Bottom-Right 1/4 */
                            else comp.snap_preview_mode = 0;
                        } else {
                            if (abs_x <= 8) comp.snap_preview_mode = 1; /* Left 1/2 */
                            else if (abs_x >= (int)screen_w - 9) comp.snap_preview_mode = 2; /* Right 1/2 */
                            else comp.snap_preview_mode = 0;
                        }

                        if (prev_mode != comp.snap_preview_mode) {
                            redraw_needed = true;
                        }
                        redraw_needed = true;
                    } else {
                        drag_wid = 0;
                        comp.snap_preview_mode = 0;
                    }
                } else if (btn_press) {
                    /* If desktop context menu is open, handle clicks on it */
                    if (comp.ctx_menu_active) {
                        if (lclick) {
                            int mx = comp.ctx_menu_x;
                            int my = comp.ctx_menu_y;
                            int mw = 176;
                            int mh = 8 * 26 + 12;
                            if (mx + mw > (int)screen_w - 8) mx = (int)screen_w - mw - 8;
                            if (my + mh > (int)screen_h - 48) my = (int)screen_h - mh - 48;
                            if (mx < 8) mx = 8;
                            if (my < 8) my = 8;

                            if (abs_x >= mx && abs_x < mx + mw && abs_y >= my + 6 && abs_y < my + 6 + 8 * 26) {
                                int item = (abs_y - (my + 6)) / 26;
                                switch (item) {
                                    case 0: az_spawn("/bin/terminal.elf"); break;
                                    case 1: az_spawn("/bin/filemanager.elf"); break;
                                    case 2: az_spawn("/bin/texteditor.elf"); break;
                                    case 3: az_spawn("/bin/calculator.elf"); break;
                                    case 4: az_spawn("/bin/paint.elf"); break;
                                    case 5: az_spawn("/bin/sysmon.elf"); break;
                                    case 6: az_spawn("/bin/settings.elf"); break;
                                    case 7: compositor_damage_all(&comp); break;
                                }
                            }
                        }
                        comp.ctx_menu_active = 0;
                        redraw_needed = true;
                    } else {
                        /* Hit-test windows front→back */
                        az_window_t *hit = find_window_at(&comp, abs_x, abs_y);
                        if (hit) {
                            /* Close button takes priority (left click only) */
                            if (lclick && hit_close_button(hit, abs_x, abs_y)) {
                                unsigned int closed_wid = hit->wid;
                                int client_chan = (int)hit->client_chan;

                                az_wm_msg_t cmsg;
                                memset(&cmsg, 0, sizeof(cmsg));
                                cmsg.type = AZ_WM_DESTROY_WINDOW;
                                cmsg.wid  = closed_wid;
                                az_channel_send_nb(client_chan, (az_ipc_msg_t *)&cmsg);

                                unsigned int prev_focus = comp.focused_window ? comp.focused_window->wid : 0;
                                compositor_destroy_window(&comp, closed_wid);
                                unsigned int new_focus = comp.focused_window ? comp.focused_window->wid : 0;
                                if (prev_focus != new_focus) {
                                    de_comp_broadcast_focus(&de_state, prev_focus, new_focus);
                                }
                                de_comp_broadcast_destroyed(&de_state, closed_wid);
                                redraw_needed = true;
                            } else if (lclick && hit_minimize_button(hit, abs_x, abs_y)) {
                                compositor_trigger_minimize_animation(&comp, hit, (int)screen_w / 2, (int)screen_h - 26);
                                if (comp.focused_window == hit) {
                                    comp.focused_window = 0;
                                    az_window_t *next_focus = comp.list_head;
                                    while (next_focus) {
                                        if (next_focus->visible) break;
                                        next_focus = next_focus->next;
                                    }
                                    if (next_focus) {
                                        compositor_focus_window(&comp, next_focus);
                                        de_comp_enforce_zorder(&comp, &de_state);
                                        de_comp_broadcast_focus(&de_state, hit->wid, next_focus->wid);
                                    } else {
                                        de_comp_broadcast_focus(&de_state, hit->wid, 0);
                                    }
                                }
                                redraw_needed = true;
                            } else if (lclick && hit_maximize_button(hit, abs_x, abs_y)) {
                                if (!hit->maximized) {
                                    hit->saved_x = hit->x;
                                    hit->saved_y = hit->y;
                                    hit->saved_w = hit->width;
                                    hit->saved_h = hit->height;
                                    hit->maximized = 1;
                                    hit->x = AZWM_BORDER_W;
                                    hit->y = AZWM_TITLEBAR_H + AZWM_BORDER_W;
                                    hit->width = screen_w - 2 * AZWM_BORDER_W;
                                    hit->height = screen_h - AZWM_TASKBAR_H - AZWM_TITLEBAR_H - 2 * AZWM_BORDER_W;
                                } else {
                                    hit->x = hit->saved_x;
                                    hit->y = hit->saved_y;
                                    hit->width = hit->saved_w;
                                    hit->height = hit->saved_h;
                                    hit->maximized = 0;
                                }
                                redraw_needed = true;
                            } else if (lclick && hit_resize_grip(hit, abs_x, abs_y)) {
                                /* Start window resizing */
                                resize_wid = hit->wid;
                                resize_start_w = (int)hit->width;
                                resize_start_h = (int)hit->height;
                                resize_start_mx = abs_x;
                                resize_start_my = abs_y;
                                if (!hit->focused) {
                                    prev_focus_wid = comp.focused_window ? comp.focused_window->wid : 0;
                                    compositor_focus_window(&comp, hit);
                                    de_comp_enforce_zorder(&comp, &de_state);
                                    de_comp_broadcast_focus(&de_state, prev_focus_wid, hit->wid);
                                    redraw_needed = true;
                                }
                            } else {
                                /* Focus the window (raise to top) if it's an application window */
                                unsigned int new_wid = hit->wid;
                                if (!hit->focused && hit->title[0] != '\0') {
                                    prev_focus_wid = comp.focused_window ? comp.focused_window->wid : 0;
                                    compositor_focus_window(&comp, hit);
                                    de_comp_enforce_zorder(&comp, &de_state);
                                    de_comp_broadcast_focus(&de_state, prev_focus_wid, new_wid);
                                    redraw_needed = true;
                                }

                                /* Forward mouse event to client or start drag */
                                if (lclick && hit_titlebar(hit, abs_x, abs_y)) {
                                    unsigned int now_time = ev.timestamp;
                                    if ((now_time - last_click_time) < 40 &&
                                        (abs_x - last_click_x) * (abs_x - last_click_x) +
                                        (abs_y - last_click_y) * (abs_y - last_click_y) < 25) {
                                        /* Double click on titlebar -> Toggle Maximize */
                                        if (!hit->maximized) {
                                            hit->saved_x = hit->x; hit->saved_y = hit->y;
                                            hit->saved_w = hit->width; hit->saved_h = hit->height;
                                            hit->maximized = 1;
                                            hit->x = AZWM_BORDER_W;
                                            hit->y = AZWM_TITLEBAR_H + AZWM_BORDER_W;
                                            hit->width = screen_w - 2 * AZWM_BORDER_W;
                                            hit->height = screen_h - AZWM_TASKBAR_H - AZWM_TITLEBAR_H - 2 * AZWM_BORDER_W;
                                        } else {
                                            hit->x = hit->saved_x; hit->y = hit->saved_y;
                                            hit->width = hit->saved_w; hit->height = hit->saved_h;
                                            hit->maximized = 0;
                                        }
                                        redraw_needed = true;
                                        drag_wid = 0;
                                        last_click_time = 0;
                                    } else {
                                        last_click_time = now_time;
                                        last_click_x = abs_x;
                                        last_click_y = abs_y;
                                        drag_wid = hit->wid;
                                        drag_off_x = abs_x - hit->x;
                                        drag_off_y = abs_y - hit->y;
                                    }
                                } else {
                                    /* Forward to client as AZ_WM_MOUSE_EVENT */
                                    az_wm_msg_t fwd;
                                    int j;
                                    for (j = 0; j < (int)sizeof(fwd); j++)
                                        ((char*)&fwd)[j] = 0;
                                    fwd.type = AZ_WM_MOUSE_EVENT;
                                    fwd.wid  = hit->wid;
                                    fwd.mouse.dx      = (short)ev.mouse_dx;
                                    fwd.mouse.dy      = (short)ev.mouse_dy;
                                    fwd.mouse.abs_x   = (short)(abs_x - hit->x);
                                    fwd.mouse.abs_y   = (short)(abs_y - hit->y);
                                    fwd.mouse.buttons = ev.mouse_buttons;
                                    int send_ret = az_channel_send_nb(hit->client_chan, (az_ipc_msg_t *)&fwd);
                                    if (send_ret == -32) {
                                        unsigned int dead_wid = hit->wid;
                                        unsigned int prev_focus = comp.focused_window ? comp.focused_window->wid : 0;
                                        compositor_destroy_window(&comp, dead_wid);
                                        unsigned int new_focus = comp.focused_window ? comp.focused_window->wid : 0;
                                        if (prev_focus != new_focus) {
                                            de_comp_broadcast_focus(&de_state, prev_focus, new_focus);
                                        }
                                        de_comp_broadcast_destroyed(&de_state, dead_wid);
                                        redraw_needed = true;
                                    }
                                }
                            }
                        } else if (rclick) {
                            /* Right-click on desktop wallpaper opens context menu */
                            comp.ctx_menu_active = 1;
                            comp.ctx_menu_x = abs_x;
                            comp.ctx_menu_y = abs_y;
                            comp.ctx_menu_hover = -1;
                            redraw_needed = true;
                        }
                    }
                } else {
                    /* Mouse move (no click): update context menu hover or forward to window under cursor */
                    if (comp.ctx_menu_active) {
                        int mx = comp.ctx_menu_x;
                        int my = comp.ctx_menu_y;
                        int mw = 176;
                        int mh = 8 * 26 + 12;
                        if (mx + mw > (int)screen_w - 8) mx = (int)screen_w - mw - 8;
                        if (my + mh > (int)screen_h - 48) my = (int)screen_h - mh - 48;
                        if (mx < 8) mx = 8;
                        if (my < 8) my = 8;

                        if (abs_x >= mx && abs_x < mx + mw && abs_y >= my + 6 && abs_y < my + 6 + 8 * 26) {
                            int item = (abs_y - (my + 6)) / 26;
                            if (item != comp.ctx_menu_hover) {
                                comp.ctx_menu_hover = item;
                                redraw_needed = true;
                            }
                        } else if (comp.ctx_menu_hover != -1) {
                            comp.ctx_menu_hover = -1;
                            redraw_needed = true;
                        }
                    }

                    az_window_t *target_win = find_window_at(&comp, abs_x, abs_y);
                    if (!target_win) target_win = comp.focused_window;

                    if (target_win) {
                        az_wm_msg_t fwd;
                        int j;
                        for (j = 0; j < (int)sizeof(fwd); j++)
                            ((char*)&fwd)[j] = 0;
                        fwd.type = AZ_WM_MOUSE_EVENT;
                        fwd.wid  = target_win->wid;
                        fwd.mouse.dx      = (short)ev.mouse_dx;
                        fwd.mouse.dy      = (short)ev.mouse_dy;
                        fwd.mouse.abs_x   = (short)(abs_x - target_win->x);
                        fwd.mouse.abs_y   = (short)(abs_y - target_win->y);
                        fwd.mouse.buttons = ev.mouse_buttons;
                        int send_ret = az_channel_send_nb(target_win->client_chan, (az_ipc_msg_t *)&fwd);
                        if (send_ret == -32) {
                            unsigned int dead_wid = target_win->wid;
                            unsigned int prev_focus = comp.focused_window ? comp.focused_window->wid : 0;
                            compositor_destroy_window(&comp, dead_wid);
                            unsigned int new_focus = comp.focused_window ? comp.focused_window->wid : 0;
                            if (prev_focus != new_focus) {
                                de_comp_broadcast_focus(&de_state, prev_focus, new_focus);
                            }
                            de_comp_broadcast_destroyed(&de_state, dead_wid);
                            redraw_needed = true;
                        }
                    }
                }

            } else if (ev.type == AZ_INPUT_EVENT_KEY) {
                bool is_alt = (ev.flags & AZ_KEY_FLAG_ALT) != 0;
                bool is_ctrl = (ev.flags & AZ_KEY_FLAG_CTRL) != 0;
                bool is_pressed = (ev.flags & AZ_KEY_FLAG_PRESSED) != 0;

                if (is_pressed) {
                    /* Alt+Tab: Interactive App Switcher */
                    if (is_alt && (ev.keycode == '\t' || ev.scancode == 0x0F)) {
                        if (!comp.alt_tab_active) {
                            comp.alt_tab_count = 0;
                            az_window_t *w = comp.list_head;
                            while (w && comp.alt_tab_count < AZWM_MAX_WINDOWS) {
                                if (w->visible && w->title[0] != '\0') {
                                    comp.alt_tab_wids[comp.alt_tab_count++] = w->wid;
                                }
                                w = w->next;
                            }
                            if (comp.alt_tab_count > 0) {
                                comp.alt_tab_active = 1;
                                comp.alt_tab_idx = (comp.alt_tab_count > 1) ? 1 : 0;
                                redraw_needed = true;
                            }
                        } else {
                            if (comp.alt_tab_count > 0) {
                                comp.alt_tab_idx = (comp.alt_tab_idx + 1) % comp.alt_tab_count;
                                redraw_needed = true;
                            }
                        }
                        continue;
                    }

                    /* Alt+F4: Close focused window */
                    if (is_alt && (ev.scancode == 0x3E || ev.keycode == 131 /* F4 */)) {
                        if (comp.focused_window && comp.focused_window->title[0] != '\0') {
                            unsigned int closed_wid = comp.focused_window->wid;
                            int client_chan = (int)comp.focused_window->client_chan;

                            az_wm_msg_t cmsg;
                            memset(&cmsg, 0, sizeof(cmsg));
                            cmsg.type = AZ_WM_DESTROY_WINDOW;
                            cmsg.wid  = closed_wid;
                            az_channel_send_nb(client_chan, (az_ipc_msg_t *)&cmsg);

                            unsigned int prev_focus = comp.focused_window ? comp.focused_window->wid : 0;
                            compositor_destroy_window(&comp, closed_wid);
                            unsigned int new_focus = comp.focused_window ? comp.focused_window->wid : 0;
                            if (prev_focus != new_focus) {
                                de_comp_broadcast_focus(&de_state, prev_focus, new_focus);
                            }
                            de_comp_broadcast_destroyed(&de_state, closed_wid);
                            redraw_needed = true;
                        }
                        continue;
                    }

                    /* Super+T or Ctrl+Alt+T: Terminal shortcut */
                    if ((ev.scancode == 0x5B || (is_ctrl && is_alt)) && (ev.keycode == 't' || ev.keycode == 'T')) {
                        az_spawn("/bin/terminal.elf");
                        continue;
                    }

                    /* Super+E or Ctrl+Alt+E: File Manager shortcut */
                    if ((ev.scancode == 0x5B || (is_ctrl && is_alt)) && (ev.keycode == 'e' || ev.keycode == 'E')) {
                        az_spawn("/bin/filemanager.elf");
                        continue;
                    }

                    /* Super+S or PrintScreen: Screenshot shortcut */
                    if ((ev.scancode == 0x37 || ev.scancode == 0x54) ||
                        ((ev.scancode == 0x5B || (is_ctrl && is_alt)) && (ev.keycode == 's' || ev.keycode == 'S'))) {
                        az_spawn("/bin/screenshot.elf");
                        continue;
                    }

                    /* Super+P: Paint tool shortcut */
                    if ((ev.scancode == 0x5B || (is_ctrl && is_alt)) && (ev.keycode == 'p' || ev.keycode == 'P')) {
                        az_spawn("/bin/paint.elf");
                        continue;
                    }

                    /* Super+I or Ctrl+Alt+I: Settings / Control Center shortcut */
                    if ((ev.scancode == 0x5B || (is_ctrl && is_alt)) && (ev.keycode == 'i' || ev.keycode == 'I')) {
                        az_spawn("/bin/settings.elf");
                        continue;
                    }

                    /* Super+C or Ctrl+Alt+C: Calculator shortcut */
                    if ((ev.scancode == 0x5B || (is_ctrl && is_alt)) && (ev.keycode == 'c' || ev.keycode == 'C')) {
                        az_spawn("/bin/calculator.elf");
                        continue;
                    }

                    /* Super+N or Ctrl+Alt+N: Text Editor shortcut */
                    if ((ev.scancode == 0x5B || (is_ctrl && is_alt)) && (ev.keycode == 'n' || ev.keycode == 'N')) {
                        az_spawn("/bin/texteditor.elf");
                        continue;
                    }

                    /* Super+D: Show Desktop (toggle minimize all) */
                    if ((ev.scancode == 0x5B || (is_ctrl && is_alt)) && (ev.keycode == 'd' || ev.keycode == 'D')) {
                        az_window_t *w = comp.list_head;
                        while (w) {
                            if (w->title[0] != '\0') {
                                w->visible = !w->visible;
                            }
                            w = w->next;
                        }
                        redraw_needed = true;
                        continue;
                    }

                    /* Super or Super+Space: Launcher shortcut */
                    if (ev.scancode == 0x5B || (is_ctrl && ev.keycode == ' ')) {
                        az_spawn("/bin/launcher.elf");
                        continue;
                    }

                    /* Alt+Left: Snap Left Half */
                    if (is_alt && (ev.scancode == 0x4B || ev.keycode == 132) && comp.focused_window && comp.focused_window->title[0] != '\0') {
                        az_window_t *fwin = comp.focused_window;
                        if (!fwin->maximized) {
                            fwin->saved_x = fwin->x; fwin->saved_y = fwin->y;
                            fwin->saved_w = fwin->width; fwin->saved_h = fwin->height;
                        }
                        fwin->maximized = 0;
                        fwin->x = AZWM_BORDER_W;
                        fwin->y = AZWM_TITLEBAR_H + AZWM_BORDER_W;
                        fwin->width = (screen_w / 2) - 2 * AZWM_BORDER_W;
                        fwin->height = screen_h - AZWM_TASKBAR_H - AZWM_TITLEBAR_H - 2 * AZWM_BORDER_W;
                        redraw_needed = true;
                        continue;
                    }

                    /* Alt+Right: Snap Right Half */
                    if (is_alt && (ev.scancode == 0x4D || ev.keycode == 133) && comp.focused_window && comp.focused_window->title[0] != '\0') {
                        az_window_t *fwin = comp.focused_window;
                        if (!fwin->maximized) {
                            fwin->saved_x = fwin->x; fwin->saved_y = fwin->y;
                            fwin->saved_w = fwin->width; fwin->saved_h = fwin->height;
                        }
                        fwin->maximized = 0;
                        fwin->x = (screen_w / 2) + AZWM_BORDER_W;
                        fwin->y = AZWM_TITLEBAR_H + AZWM_BORDER_W;
                        fwin->width = (screen_w / 2) - 2 * AZWM_BORDER_W;
                        fwin->height = screen_h - AZWM_TASKBAR_H - AZWM_TITLEBAR_H - 2 * AZWM_BORDER_W;
                        redraw_needed = true;
                        continue;
                    }

                    /* Alt+Up: Maximize */
                    if (is_alt && (ev.scancode == 0x48 || ev.keycode == 134) && comp.focused_window && comp.focused_window->title[0] != '\0') {
                        az_window_t *fwin = comp.focused_window;
                        if (!fwin->maximized) {
                            fwin->saved_x = fwin->x; fwin->saved_y = fwin->y;
                            fwin->saved_w = fwin->width; fwin->saved_h = fwin->height;
                            fwin->maximized = 1;
                            fwin->x = AZWM_BORDER_W;
                            fwin->y = AZWM_TITLEBAR_H + AZWM_BORDER_W;
                            fwin->width = screen_w - 2 * AZWM_BORDER_W;
                            fwin->height = screen_h - AZWM_TASKBAR_H - AZWM_TITLEBAR_H - 2 * AZWM_BORDER_W;
                            redraw_needed = true;
                        }
                        continue;
                    }

                    /* Alt+Down: Restore */
                    if (is_alt && (ev.scancode == 0x50 || ev.keycode == 135) && comp.focused_window && comp.focused_window->title[0] != '\0') {
                        az_window_t *fwin = comp.focused_window;
                        if (fwin->maximized) {
                            fwin->x = fwin->saved_x; fwin->y = fwin->saved_y;
                            fwin->width = fwin->saved_w; fwin->height = fwin->saved_h;
                            fwin->maximized = 0;
                            redraw_needed = true;
                        }
                        continue;
                    }
                } else {
                    /* Key released: If Alt+Tab was active and Alt is released, switch to selected window */
                    if (comp.alt_tab_active && !is_alt) {
                        comp.alt_tab_active = 0;
                        if (comp.alt_tab_count > 0 && comp.alt_tab_idx < comp.alt_tab_count) {
                            unsigned int target_wid = comp.alt_tab_wids[comp.alt_tab_idx];
                            az_window_t *twin = (az_window_t *)0;
                            for (int k = 0; k < AZWM_MAX_WINDOWS; k++) {
                                if (comp.window_pool[k].wid == target_wid) {
                                    twin = &comp.window_pool[k];
                                    break;
                                }
                            }
                            if (twin) {
                                unsigned int prev = comp.focused_window ? comp.focused_window->wid : 0;
                                compositor_focus_window(&comp, twin);
                                de_comp_enforce_zorder(&comp, &de_state);
                                de_comp_broadcast_focus(&de_state, prev, twin->wid);
                            }
                        }
                        redraw_needed = true;
                    }
                }

                /* Forward normal key event to focused application window */
                az_window_t *kw = comp.focused_window;
                if (!kw || kw->title[0] == '\0' || !kw->visible) {
                    az_window_t *curr = comp.list_head;
                    while (curr) {
                        if (curr->wid != 0 && curr->visible && curr->title[0] != '\0') {
                            kw = curr;
                            break;
                        }
                        curr = curr->next;
                    }
                }

                if (kw) {
                    az_wm_msg_t fwd;
                    int j;
                    for (j = 0; j < (int)sizeof(fwd); j++)
                        ((char*)&fwd)[j] = 0;
                    fwd.type = AZ_WM_KEY_EVENT;
                    fwd.wid  = kw->wid;
                    fwd.key.keycode   = ev.keycode;
                    fwd.key.scancode  = ev.scancode;
                    fwd.key.pressed   = (ev.flags & AZ_KEY_FLAG_PRESSED) ? 1 : 0;
                    fwd.key.modifiers = (unsigned short)(ev.flags >> 2);
                    int send_ret = az_channel_send_nb(kw->client_chan, (az_ipc_msg_t *)&fwd);
                    if (send_ret == -32) {
                        unsigned int dead_wid = kw->wid;
                        unsigned int prev_focus = comp.focused_window ? comp.focused_window->wid : 0;
                        compositor_destroy_window(&comp, dead_wid);
                        unsigned int new_focus = comp.focused_window ? comp.focused_window->wid : 0;
                        if (prev_focus != new_focus) {
                            de_comp_broadcast_focus(&de_state, prev_focus, new_focus);
                        }
                        de_comp_broadcast_destroyed(&de_state, dead_wid);
                        redraw_needed = true;
                    }
                }
            }
        } /* end input poll loop */

        /* ── 2. Process ALL pending IPC messages (non-blocking) ─────────── */
        az_wm_msg_t msg;
        while (az_channel_recv_nb(server_chan, (az_ipc_msg_t *)&msg) == 0) {
            switch (msg.type) {

            case AZ_WM_CREATE_WINDOW: {
                unsigned int shmem_id = 0;
                int wid = compositor_create_window(&comp,
                                                   0, /* owner_pid */
                                                   msg.client_chan,
                                                   msg.create.x, msg.create.y,
                                                   msg.create.w, msg.create.h,
                                                   msg.create.title,
                                                   &shmem_id);
                if (wid > 0) {
                    /* Reply to client */
                    az_wm_msg_t reply;
                    int j;
                    for (j = 0; j < (int)sizeof(reply); j++)
                        ((char*)&reply)[j] = 0;
                    reply.type = AZ_WM_WINDOW_CREATED;
                    reply.created.shmem_id     = shmem_id;
                    reply.created.assigned_wid = (unsigned int)wid;
                    reply.created.width        = msg.create.w;
                    reply.created.height       = msg.create.h;
                    az_channel_send_nb(msg.client_chan, (az_ipc_msg_t *)&reply);

                    /* Broadcast to DE subscribers */
                    az_window_t *win = (az_window_t *)0;
                    unsigned int i;
                    for (i = 0; i < AZWM_MAX_WINDOWS; i++) {
                        if (comp.window_pool[i].wid == (unsigned int)wid) {
                            win = &comp.window_pool[i];
                            break;
                        }
                    }
                    if (win) {
                        int slot = (int)(win - comp.window_pool);
                        if (slot >= 0 && slot < AZWM_MAX_WINDOWS) {
                            de_state.zorder_hints[slot] = AZ_WM_ZORDER_NORMAL;
                        }
                        de_comp_broadcast_created(&de_state, win);
                        de_comp_enforce_zorder(&comp, &de_state);
                    }

                    redraw_needed = true;
                } else {
                    /* Reply with failure */
                    az_wm_msg_t reply;
                    int j;
                    for (j = 0; j < (int)sizeof(reply); j++)
                        ((char*)&reply)[j] = 0;
                    reply.type = AZ_WM_WINDOW_CREATED;
                    reply.created.shmem_id     = 0;
                    reply.created.assigned_wid = 0;
                    reply.created.width        = 0;
                    reply.created.height       = 0;
                    az_channel_send_nb(msg.client_chan, (az_ipc_msg_t *)&reply);
                    de_log("[azwm] WARNING: Window creation failed, maximum windows reached or shmem full");
                }
                break;
            }

            case AZ_WM_DESTROY_WINDOW: {
                unsigned int closed_wid = msg.wid;
                unsigned int prev_focus = comp.focused_window ? comp.focused_window->wid : 0;
                compositor_destroy_window(&comp, closed_wid);
                unsigned int new_focus = comp.focused_window ? comp.focused_window->wid : 0;
                if (prev_focus != new_focus) {
                    de_comp_broadcast_focus(&de_state, prev_focus, new_focus);
                }
                de_comp_broadcast_destroyed(&de_state, closed_wid);
                redraw_needed = true;
                break;
            }

            case AZ_WM_INVALIDATE: {
                az_window_t *iwin = (az_window_t *)0;
                for (unsigned int i = 0; i < AZWM_MAX_WINDOWS; i++) {
                    if (comp.window_pool[i].wid == msg.wid) {
                        iwin = &comp.window_pool[i];
                        break;
                    }
                }
                if (iwin && iwin->visible) {
                    compositor_damage(&comp, iwin->x - AZWM_BORDER_W, iwin->y - AZWM_TITLEBAR_H - AZWM_BORDER_W,
                                      (int)iwin->width + 2 * AZWM_BORDER_W, (int)iwin->height + AZWM_TITLEBAR_H + 2 * AZWM_BORDER_W);
                } else {
                    compositor_damage_all(&comp);
                }
                redraw_needed = true;
                break;
            }

            case AZ_WM_MOVE_WINDOW: {
                unsigned int i;
                for (i = 0; i < AZWM_MAX_WINDOWS; i++) {
                    if (comp.window_pool[i].wid == msg.wid) {
                        int old_x = comp.window_pool[i].x;
                        int old_y = comp.window_pool[i].y;
                        int new_x = msg.move.x;
                        int new_y = msg.move.y;
                        int margin = 16;
                        compositor_damage(&comp, old_x - AZWM_BORDER_W - margin, old_y - AZWM_TITLEBAR_H - AZWM_BORDER_W - margin,
                                          (int)comp.window_pool[i].width + 2 * AZWM_BORDER_W + 2 * margin, (int)comp.window_pool[i].height + AZWM_TITLEBAR_H + 2 * AZWM_BORDER_W + 2 * margin);
                        comp.window_pool[i].x = new_x;
                        comp.window_pool[i].y = new_y;
                        compositor_damage(&comp, new_x - AZWM_BORDER_W - margin, new_y - AZWM_TITLEBAR_H - AZWM_BORDER_W - margin,
                                          (int)comp.window_pool[i].width + 2 * AZWM_BORDER_W + 2 * margin, (int)comp.window_pool[i].height + AZWM_TITLEBAR_H + 2 * AZWM_BORDER_W + 2 * margin);
                        redraw_needed = true;
                        break;
                    }
                }
                break;
            }

            case AZ_WM_RESTORE_WINDOW: {
                unsigned int i;
                for (i = 0; i < AZWM_MAX_WINDOWS; i++) {
                    if (comp.window_pool[i].wid == msg.wid) {
                        comp.window_pool[i].visible = 1;
                        prev_focus_wid = comp.focused_window ? comp.focused_window->wid : 0;
                        compositor_focus_window(&comp, &comp.window_pool[i]);
                        de_comp_enforce_zorder(&comp, &de_state);
                        de_comp_broadcast_focus(&de_state, prev_focus_wid, msg.wid);
                        redraw_needed = true;
                        break;
                    }
                }
                break;
            }

            case AZ_WM_MINIMIZE_WINDOW: {
                unsigned int i;
                for (i = 0; i < AZWM_MAX_WINDOWS; i++) {
                    if (comp.window_pool[i].wid == msg.wid) {
                        compositor_trigger_minimize_animation(&comp, &comp.window_pool[i], (int)screen_w / 2, (int)screen_h - 26);
                        if (comp.focused_window == &comp.window_pool[i]) {
                            comp.focused_window = 0;
                            az_window_t *next_focus = comp.list_head;
                            while (next_focus) {
                                if (next_focus->visible) break;
                                next_focus = next_focus->next;
                            }
                            if (next_focus) {
                                compositor_focus_window(&comp, next_focus);
                                de_comp_enforce_zorder(&comp, &de_state);
                                de_comp_broadcast_focus(&de_state, msg.wid, next_focus->wid);
                            } else {
                                de_comp_broadcast_focus(&de_state, msg.wid, 0);
                            }
                        }
                        redraw_needed = true;
                        break;
                    }
                }
                break;
            }

            /* ── DE protocol messages ──────────────────────────────────── */
            case AZ_WM_SET_ZORDER_HINT:
            case AZ_WM_SUBSCRIBE_EVENTS:
            case AZ_WM_UNSUBSCRIBE_EVENTS:
            case AZ_WM_LAUNCH_APP:
            case AZ_WM_SET_STRUT:
            case AZ_WM_SET_THEME:
                if (de_comp_handle_message(&comp, &de_state, &msg))
                    redraw_needed = true;
                break;

            default:
                break;
            }
        }

        /* ── Animate active window transitions ─────────────────────────── */
        if (comp.has_animating_windows) {
            if (compositor_animate_step(&comp)) {
                redraw_needed = true;
            }
        }

        if (redraw_needed) {
            compose_screen(&comp);
        } else if (cursor_moved) {
            compositor_update_cursor(&comp);
        }

        if (!redraw_needed && !cursor_moved && !comp.has_animating_windows) {
            usleep(2000);
        }
    }

    sys_exit(0);
}
