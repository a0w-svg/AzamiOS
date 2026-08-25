/* ============================================================================
 * AzamiOS — Display Server Compositor Implementation
 * File: user/apps/azwm/compositor.c
 * ============================================================================ */

#include "compositor.h"
#include "de_protocol.h"
#include "desktop.h"
#include "../../libc/include/az/ipc.h"
#include "../../libc/include/string.h"
#include "../../libc/include/stdio.h"
#include <stdbool.h>

/* ── Pixel buffer base address for shared memory window buffers ───────────── */
#define SHMEM_WINDOW_BASE  0x50000000UL
#define SHMEM_WINDOW_STEP  0x01000000UL  /* 16 MB per window */

/* ── Initialization ──────────────────────────────────────────────────────── */

void compositor_init(az_compositor_t *comp,
                     unsigned int *frontbuf,
                     unsigned int *backbuf,
                     unsigned int w, unsigned int h, unsigned int pitch,
                     int server_channel)
{
    comp->fb_width       = w;
    comp->fb_height      = h;
    comp->fb_pitch       = pitch;
    comp->window_count   = 0;
    comp->next_wid       = 1;
    comp->focused_window = 0; /* NULL */
    comp->cursor_x       = (int)(w / 2);
    comp->cursor_y       = h / 2;
    comp->old_cursor_x   = comp->cursor_x;
    comp->old_cursor_y   = comp->cursor_y;
    comp->server_channel = server_channel;
    comp->has_animating_windows = 0;

    /* Setup hardware double-buffered VRAM pointers */
    comp->frontbuf = frontbuf;
    comp->backbuf  = backbuf;
    comp->hw_page_flip = 0;

    comp->list_head = 0; /* NULL */
    comp->list_tail = 0; /* NULL */
    
    /* Initialize memory pool and string them into free_list */
    comp->free_list = 0; /* NULL */
    for (int i = AZWM_MAX_WINDOWS - 1; i >= 0; i--) {
        comp->window_pool[i].wid = 0;
        comp->window_pool[i].next = comp->free_list;
        comp->window_pool[i].prev = 0; /* NULL */
        comp->free_list = &comp->window_pool[i];
    }

    compositor_damage_all(comp);
}

void compositor_damage(az_compositor_t *comp, int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0) return;
    int x1 = x + w;
    int y1 = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x1 > (int)comp->fb_width) x1 = (int)comp->fb_width;
    if (y1 > (int)comp->fb_height) y1 = (int)comp->fb_height;
    if (x >= x1 || y >= y1) return;

    if (!comp->has_damage) {
        comp->dirty_min_x = x;
        comp->dirty_min_y = y;
        comp->dirty_max_x = x1;
        comp->dirty_max_y = y1;
        comp->has_damage = 1;
    } else {
        if (x < comp->dirty_min_x) comp->dirty_min_x = x;
        if (y < comp->dirty_min_y) comp->dirty_min_y = y;
        if (x1 > comp->dirty_max_x) comp->dirty_max_x = x1;
        if (y1 > comp->dirty_max_y) comp->dirty_max_y = y1;
    }
}

void compositor_damage_all(az_compositor_t *comp)
{
    comp->dirty_min_x = 0;
    comp->dirty_min_y = 0;
    comp->dirty_max_x = (int)comp->fb_width;
    comp->dirty_max_y = (int)comp->fb_height;
    comp->has_damage = 1;
}

/* ── Linked List Helpers ─────────────────────────────────────────────────── */
static void list_push_front(az_compositor_t *comp, az_window_t *win)
{
    win->prev = 0;
    win->next = comp->list_head;
    if (comp->list_head) {
        comp->list_head->prev = win;
    } else {
        comp->list_tail = win;
    }
    comp->list_head = win;
}

static void list_remove(az_compositor_t *comp, az_window_t *win)
{
    if (win->prev) {
        win->prev->next = win->next;
    } else {
        comp->list_head = win->next;
    }
    
    if (win->next) {
        win->next->prev = win->prev;
    } else {
        comp->list_tail = win->prev;
    }
    win->next = 0;
    win->prev = 0;
}

/* ── Window Creation ─────────────────────────────────────────────────────── */

int compositor_create_window(az_compositor_t *comp,
                             unsigned int owner_pid,
                             unsigned int client_chan,
                             int x, int y,
                             unsigned int w, unsigned int h,
                             const char *title,
                             unsigned int *out_shmem_id)
{
    if (!comp->free_list) return -1; /* No free slots */
    
    az_window_t *win = comp->free_list;
    comp->free_list = win->next;

    /* Clamp window dimensions */
    if (w < 100) w = 100;
    if (h < 60)  h = 60;
    if (w > comp->fb_width)  w = comp->fb_width;
    if (h > comp->fb_height) h = comp->fb_height;

    /* Calculate shared memory needed for pixel buffer */
    unsigned long buf_size = (unsigned long)w * h * 4;
    unsigned long page_count = (buf_size + 4095) / 4096;
    if (page_count == 0) page_count = 1;

    /* Create shared memory for the window's pixel buffer */
    int shmem_id = az_shmem_create((int)page_count);
    if (shmem_id < 0) {
        /* Return to free list */
        win->next = comp->free_list;
        comp->free_list = win;
        return -1;
    }

    /* Map the shared memory into azwm's address space */
    /* Find slot index for VA offset */
    int slot = (int)(win - comp->window_pool);
    unsigned long map_addr = SHMEM_WINDOW_BASE + (unsigned long)slot * SHMEM_WINDOW_STEP;
    int ret = az_shmem_map(shmem_id, (void *)map_addr);
    if (ret < 0) {
        az_shmem_destroy(shmem_id);
        win->next = comp->free_list;
        comp->free_list = win;
        return -1;
    }

    win->wid         = comp->next_wid++;
    win->owner_pid   = owner_pid;
    win->client_chan = client_chan;
    win->x           = x;
    win->y           = y;
    win->width       = w;
    win->height      = h;
    win->buffer_w    = w;
    win->buffer_h    = h;
    win->pixels      = (unsigned int *)map_addr;
    win->shmem_id    = (unsigned int)shmem_id;
    win->shm_bytes   = page_count * 4096;
    win->visible     = 1;
    win->focused     = 0;
    win->anim_state  = AZWM_ANIM_NONE;
    win->anim_step   = 0;

    int ti = 0;
    if (title) {
        while (title[ti] && ti < 63) { win->title[ti] = title[ti]; ti++; }
    }
    win->title[ti] = '\0';

    unsigned long pixels_total = (unsigned long)w * h;
    for (unsigned long i = 0; i < pixels_total; i++) {
        win->pixels[i] = 0xFF1E1E2E; 
    }

    /* Add to Z-order front */
    list_push_front(comp, win);
    comp->window_count++;

    if (out_shmem_id) *out_shmem_id = (unsigned int)shmem_id;

    compositor_focus_window(comp, win);
    if (win->title[0] != '\0') {
        compositor_trigger_open_animation(comp, win);
    }
    return (int)win->wid;
}

/* ── Window Destruction ──────────────────────────────────────────────────── */

void compositor_destroy_window(az_compositor_t *comp, unsigned int wid)
{
    az_window_t *curr = comp->list_head;
    while (curr) {
        if (curr->wid == wid) {
            int dmg_x = curr->x - AZWM_BORDER_W - 4;
            int dmg_y = curr->y - (AZWM_TITLEBAR_H + AZWM_BORDER_W) - 4;
            int dmg_w = (int)curr->width + 2 * AZWM_BORDER_W + 8;
            int dmg_h = (int)curr->height + AZWM_TITLEBAR_H + 2 * AZWM_BORDER_W + 8;
            compositor_damage(comp, dmg_x, dmg_y, dmg_w, dmg_h);

            if (curr->pixels) {
                az_shmem_unmap((int)curr->shmem_id, curr->pixels);
                curr->pixels = 0;
            }
            if (curr->shmem_id) {
                az_shmem_destroy((int)curr->shmem_id);
                curr->shmem_id = 0;
            }
            list_remove(comp, curr);
            curr->wid = 0;
            curr->visible = 0;
            comp->window_count--;
            if (comp->focused_window == curr) {
                comp->focused_window = 0;
                az_window_t *next_focus = comp->list_head;
                while (next_focus) {
                    if (next_focus->visible) break;
                    next_focus = next_focus->next;
                }
                if (next_focus) {
                    compositor_focus_window(comp, next_focus);
                }
            }
            curr->next = comp->free_list;
            comp->free_list = curr;
            break;
        }
        curr = curr->next;
    }
}

/* ── Hit testing ─────────────────────────────────────────────────────────── */

int compositor_find_window_at(az_compositor_t *comp, int sx, int sy)
{
    az_window_t *curr = comp->list_head;
    while (curr) {
        if (!curr->visible || curr->wid == 0) {
            curr = curr->next;
            continue;
        }

        bool has_frame = (curr->title[0] != '\0');
        int wx = curr->x;
        int wy = curr->y;
        int ww = (int)curr->width;
        int wh = (int)curr->height;

        if (has_frame) {
            wx -= AZWM_BORDER_W;
            wy -= (AZWM_TITLEBAR_H + AZWM_BORDER_W);
            ww += 2 * AZWM_BORDER_W;
            wh += (AZWM_TITLEBAR_H + 2 * AZWM_BORDER_W);
        }

        if (sx >= wx && sx < wx + ww && sy >= wy && sy < wy + wh) {
            return (int)(curr - comp->window_pool);
        }
        curr = curr->next;
    }
    return -1;
}

/* ── Focus management ─────────────────────────────────────────────────────── */

void compositor_focus_window(az_compositor_t *comp, az_window_t *win)
{
    if (comp->focused_window) {
        comp->focused_window->focused = 0;
    }
    comp->focused_window = win;
    if (win) {
        win->focused = 1;
        /* Move to top of Z-order */
        if (comp->list_head != win) {
            list_remove(comp, win);
            list_push_front(comp, win);
        }
    }
}

/* ── Rendering ───────────────────────────────────────────────────────────── */

static inline void bb_put_pixel(az_compositor_t *comp, int x, int y, unsigned int color)
{
    if (x >= 0 && x < (int)comp->fb_width && y >= 0 && y < (int)comp->fb_height) {
        unsigned int pitch_px = comp->fb_pitch / 4;
        comp->backbuf[(unsigned int)y * pitch_px + (unsigned int)x] = color;
    }
}

static inline unsigned int alpha_blend(unsigned int dst, unsigned int src, unsigned int alpha)
{
    if (alpha == 0) return dst;
    if (alpha >= 255) return src;
    unsigned int a = alpha;
    unsigned int inv_a = 255 - a;
    unsigned int rb = (unsigned int)((((unsigned long long)(src & 0x00FF00FF) * a +
                                       (unsigned long long)(dst & 0x00FF00FF) * inv_a) >> 8) & 0x00FF00FFU);
    unsigned int g  = (unsigned int)((((unsigned long long)(src & 0x0000FF00) * a +
                                       (unsigned long long)(dst & 0x0000FF00) * inv_a) >> 8) & 0x0000FF00U);
    return 0xFF000000 | rb | g;
}

/* Highly-optimized shading for black drop-shadows (avoids src arithmetic) */
static inline unsigned int alpha_shade_black(unsigned int dst, unsigned int alpha)
{
    if (alpha == 0) return dst;
    if (alpha >= 255) return 0xFF000000;
    unsigned int inv_a = 255 - alpha;
    unsigned int rb = (((dst & 0x00FF00FF) * inv_a) >> 8) & 0x00FF00FFU;
    unsigned int g  = (((dst & 0x0000FF00) * inv_a) >> 8) & 0x0000FF00U;
    return 0xFF000000 | rb | g;
}

static void bb_fill_rect(az_compositor_t *comp, int rx, int ry, int rw, int rh, unsigned int color)
{
    if (rw <= 0 || rh <= 0) return;
    int x0 = rx < 0 ? 0 : rx;
    int y0 = ry < 0 ? 0 : ry;
    int x1 = rx + rw;
    int y1 = ry + rh;
    if (x1 > (int)comp->fb_width)  x1 = (int)comp->fb_width;
    if (y1 > (int)comp->fb_height) y1 = (int)comp->fb_height;
    if (x0 >= x1 || y0 >= y1) return;

    unsigned int pitch_px = comp->fb_pitch / 4;
    int fill_w = x1 - x0;
    unsigned long long col64 = ((unsigned long long)color << 32) | (unsigned long long)color;

    for (int y = y0; y < y1; y++) {
        unsigned int *dst = &comp->backbuf[(unsigned int)y * pitch_px + (unsigned int)x0];
        int w = fill_w;
        if (((unsigned long)dst & 7) && w > 0) {
            *dst++ = color;
            w--;
        }
        unsigned long long *dst64 = (unsigned long long *)dst;
        while (w >= 4) {
            dst64[0] = col64;
            dst64[1] = col64;
            dst64 += 2;
            w -= 4;
        }
        if (w >= 2) {
            *dst64++ = col64;
            w -= 2;
        }
        dst = (unsigned int *)dst64;
        if (w > 0) {
            *dst = color;
        }
    }
}

/* High-speed scanline span circle rasterizer */
static void bb_fill_circle(az_compositor_t *comp, int cx, int cy, int r, unsigned int color)
{
    if (r <= 0) return;
    int r2 = r * r;
    unsigned int pitch_px = comp->fb_pitch / 4;
    int fb_w = (int)comp->fb_width;
    int fb_h = (int)comp->fb_height;

    for (int y = -r; y <= r; y++) {
        int py = cy + y;
        if (py < 0 || py >= fb_h) continue;
        int rem = r2 - y * y;
        if (rem < 0) continue;
        int max_x = 0;
        while ((max_x + 1) * (max_x + 1) <= rem) max_x++;

        int x0 = cx - max_x;
        int x1 = cx + max_x + 1;
        if (x0 < 0) x0 = 0;
        if (x1 > fb_w) x1 = fb_w;
        if (x0 >= x1) continue;

        unsigned int *dst = &comp->backbuf[(unsigned int)py * pitch_px + (unsigned int)x0];
        int w = x1 - x0;
        unsigned long long col64 = ((unsigned long long)color << 32) | (unsigned long long)color;
        if (((unsigned long)dst & 7) && w > 0) {
            *dst++ = color;
            w--;
        }
        unsigned long long *dst64 = (unsigned long long *)dst;
        while (w >= 4) {
            dst64[0] = col64;
            dst64[1] = col64;
            dst64 += 2;
            w -= 4;
        }
        if (w >= 2) {
            *dst64++ = col64;
            w -= 2;
        }
        dst = (unsigned int *)dst64;
        if (w > 0) *dst = color;
    }
}

static void bb_fill_rounded_rect(az_compositor_t *comp, int rx, int ry, int rw, int rh, int radius, unsigned int color)
{
    if (rw <= 0 || rh <= 0) return;
    if (radius * 2 > rw) radius = rw / 2;
    if (radius * 2 > rh) radius = rh / 2;
    if (radius <= 0) {
        bb_fill_rect(comp, rx, ry, rw, rh, color);
        return;
    }
    bb_fill_rect(comp, rx + radius, ry, rw - 2 * radius, rh, color);
    bb_fill_rect(comp, rx, ry + radius, radius, rh - 2 * radius, color);
    bb_fill_rect(comp, rx + rw - radius, ry + radius, radius, rh - 2 * radius, color);
    bb_fill_circle(comp, rx + radius, ry + radius, radius, color);
    bb_fill_circle(comp, rx + rw - radius - 1, ry + radius, radius, color);
    bb_fill_circle(comp, rx + radius, ry + rh - radius - 1, radius, color);
    bb_fill_circle(comp, rx + rw - radius - 1, ry + rh - radius - 1, radius, color);
}

static void draw_drop_shadow(az_compositor_t *comp, int rx, int ry, int rw, int rh)
{
    if (!comp || !comp->backbuf) return;
    int s = 12;
    unsigned int pitch_px = comp->fb_pitch / 4;
    int fb_w = (int)comp->fb_width;
    int fb_h = (int)comp->fb_height;

    unsigned int alpha_lut[16];
    for (int i = 0; i < s; i++) {
        int a = 120 - (i * 120 / s);
        alpha_lut[i] = (a > 0) ? (unsigned int)a : 0;
    }

    int x_start = rx < 0 ? 0 : rx;
    int x_end   = rx + rw > fb_w ? fb_w : rx + rw;
    int y_start = ry < 0 ? 0 : ry;
    int y_end   = ry + rh > fb_h ? fb_h : ry + rh;

    /* 1. Top strip */
    for (int y = ry - s; y < ry; y++) {
        if (y < 0 || y >= fb_h) continue;
        int dist = ry - y;
        if (dist >= s) continue;
        unsigned int alpha = alpha_lut[dist];
        unsigned int *line = &comp->backbuf[y * pitch_px];
        for (int x = x_start; x < x_end; x++) line[x] = alpha_shade_black(line[x], alpha);
    }

    /* 2. Bottom strip */
    for (int y = ry + rh; y < ry + rh + s; y++) {
        if (y < 0 || y >= fb_h) continue;
        int dist = y - (ry + rh) + 1;
        if (dist >= s) continue;
        unsigned int alpha = alpha_lut[dist];
        unsigned int *line = &comp->backbuf[y * pitch_px];
        for (int x = x_start; x < x_end; x++) line[x] = alpha_shade_black(line[x], alpha);
    }

    /* 3. Left strip */
    for (int x = rx - s; x < rx; x++) {
        if (x < 0 || x >= fb_w) continue;
        int dist = rx - x;
        if (dist >= s) continue;
        unsigned int alpha = alpha_lut[dist];
        for (int y = y_start; y < y_end; y++) {
            unsigned int *p = &comp->backbuf[y * pitch_px + x];
            *p = alpha_shade_black(*p, alpha);
        }
    }

    /* 4. Right strip */
    for (int x = rx + rw; x < rx + rw + s; x++) {
        if (x < 0 || x >= fb_w) continue;
        int dist = x - (rx + rw) + 1;
        if (dist >= s) continue;
        unsigned int alpha = alpha_lut[dist];
        for (int y = y_start; y < y_end; y++) {
            unsigned int *p = &comp->backbuf[y * pitch_px + x];
            *p = alpha_shade_black(*p, alpha);
        }
    }

    /* 5. Four Corners */
    for (int y = ry - s; y < ry; y++) {
        if (y < 0 || y >= fb_h) continue;
        int dy = ry - y;
        for (int x = rx - s; x < rx; x++) {
            if (x < 0 || x >= fb_w) continue;
            int dx = rx - x;
            int dist = (dx > dy) ? dx : dy;
            if (dist < s) {
                unsigned int *p = &comp->backbuf[y * pitch_px + x];
                *p = alpha_shade_black(*p, alpha_lut[dist]);
            }
        }
        for (int x = rx + rw; x < rx + rw + s; x++) {
            if (x < 0 || x >= fb_w) continue;
            int dx = x - (rx + rw) + 1;
            int dist = (dx > dy) ? dx : dy;
            if (dist < s) {
                unsigned int *p = &comp->backbuf[y * pitch_px + x];
                *p = alpha_shade_black(*p, alpha_lut[dist]);
            }
        }
    }

    for (int y = ry + rh; y < ry + rh + s; y++) {
        if (y < 0 || y >= fb_h) continue;
        int dy = y - (ry + rh) + 1;
        for (int x = rx - s; x < rx; x++) {
            if (x < 0 || x >= fb_w) continue;
            int dx = rx - x;
            int dist = (dx > dy) ? dx : dy;
            if (dist < s) {
                unsigned int *p = &comp->backbuf[y * pitch_px + x];
                *p = alpha_shade_black(*p, alpha_lut[dist]);
            }
        }
        for (int x = rx + rw; x < rx + rw + s; x++) {
            if (x < 0 || x >= fb_w) continue;
            int dx = x - (rx + rw) + 1;
            int dist = (dx > dy) ? dx : dy;
            if (dist < s) {
                unsigned int *p = &comp->backbuf[y * pitch_px + x];
                *p = alpha_shade_black(*p, alpha_lut[dist]);
            }
        }
    }
}

static void render_window(az_compositor_t *comp, az_window_t *win)
{
    if (!win->visible || win->wid == 0) return;

    bool has_frame = (win->title[0] != '\0');

    int wx = win->x;
    int wy = win->y;
    int ww = (int)win->width;
    int wh = (int)win->height;

    if (has_frame) {
        if (win->focused) {
            draw_drop_shadow(comp, wx - AZWM_BORDER_W, wy - AZWM_TITLEBAR_H - AZWM_BORDER_W,
                             ww + 2 * AZWM_BORDER_W, wh + AZWM_TITLEBAR_H + 2 * AZWM_BORDER_W);
        }

        /* Border / frame: radius 10, focused = blue tint, unfocused = surface */
        unsigned int border_color = win->focused ? 0xFF89B4FA : 0xFF45475A;
        bb_fill_rounded_rect(comp, wx - AZWM_BORDER_W, wy - AZWM_TITLEBAR_H - AZWM_BORDER_W,
                             ww + 2 * AZWM_BORDER_W, wh + AZWM_TITLEBAR_H + 2 * AZWM_BORDER_W, 10, border_color);

        /* Gradient titlebar */
        {
            unsigned int pitch_px2 = comp->fb_pitch / 4;
            int tb_top_y = wy - AZWM_TITLEBAR_H;
            int tb_bot_y = wy + 6;
            int tb_range = tb_bot_y - tb_top_y;
            for (int ty2 = tb_top_y; ty2 < tb_bot_y; ty2++) {
                if (ty2 < 0 || ty2 >= (int)comp->fb_height) continue;
                unsigned int t = (tb_range > 1) ? (unsigned int)((ty2 - tb_top_y) * 255 / (tb_range - 1)) : 0;
                unsigned int col;
                if (win->focused) {
                    unsigned int r2 = (0x25 * (255 - t) + 0x31 * t) / 255;
                    unsigned int g2 = (0x25 * (255 - t) + 0x32 * t) / 255;
                    unsigned int b2 = (0x35 * (255 - t) + 0x44 * t) / 255;
                    col = 0xFF000000 | (r2 << 16) | (g2 << 8) | b2;
                } else {
                    unsigned int r2 = (0x18 * (255 - t) + 0x1E * t) / 255;
                    unsigned int g2 = (0x18 * (255 - t) + 0x1E * t) / 255;
                    unsigned int b2 = (0x28 * (255 - t) + 0x2E * t) / 255;
                    col = 0xFF000000 | (r2 << 16) | (g2 << 8) | b2;
                }
                for (int tx2 = wx; tx2 < wx + ww; tx2++) {
                    if (tx2 < 0 || tx2 >= (int)comp->fb_width) continue;
                    comp->backbuf[ty2 * pitch_px2 + tx2] = col;
                }
            }

            /* Specular glass highlight line on top edge of titlebar */
            if (tb_top_y >= 0 && tb_top_y < (int)comp->fb_height) {
                unsigned int highlight = win->focused ? 0x60FFFFFF : 0x25FFFFFF;
                unsigned int *hl_line = &comp->backbuf[tb_top_y * pitch_px2];
                for (int tx2 = wx + 2; tx2 < wx + ww - 2; tx2++) {
                    if (tx2 >= 0 && tx2 < (int)comp->fb_width) {
                        hl_line[tx2] = alpha_blend(hl_line[tx2], 0xFFFFFFFF, (highlight >> 24) & 0xFF);
                    }
                }
            }
        }

        /* Window title text (with subtle drop shadow) */
        {
            int tx = wx + 8;
            int ty = wy - AZWM_TITLEBAR_H + 4;
            /* Title shadow */
            for (int i = 0; win->title[i] && tx + 8 < wx + ww - 65; i++) {
                desktop_draw_char_at(comp->backbuf, comp->fb_width, comp->fb_height,
                                     comp->fb_pitch / 4, tx + 1, ty + 1, win->title[i],
                                     0xFF11111B);
                desktop_draw_char_at(comp->backbuf, comp->fb_width, comp->fb_height,
                                     comp->fb_pitch / 4, tx, ty, win->title[i],
                                     win->focused ? 0xFFCDD6F4 : 0xFF6C7086);
                tx += 8;
            }
        }

        /* Decoration buttons: radius 7, with glyphs */
        /* Close button — red circle with × glyph */
        int close_x = wx + ww - 18;
        int close_y = wy - AZWM_TITLEBAR_H + 12;
        bb_fill_circle(comp, close_x, close_y, 7, 0xFFF38BA8);
        bb_put_pixel(comp, close_x - 2, close_y - 2, 0xFF1E1E2E);
        bb_put_pixel(comp, close_x - 1, close_y - 1, 0xFF1E1E2E);
        bb_put_pixel(comp, close_x,     close_y,     0xFF1E1E2E);
        bb_put_pixel(comp, close_x + 1, close_y + 1, 0xFF1E1E2E);
        bb_put_pixel(comp, close_x + 2, close_y + 2, 0xFF1E1E2E);
        bb_put_pixel(comp, close_x + 2, close_y - 2, 0xFF1E1E2E);
        bb_put_pixel(comp, close_x + 1, close_y - 1, 0xFF1E1E2E);
        bb_put_pixel(comp, close_x - 1, close_y + 1, 0xFF1E1E2E);
        bb_put_pixel(comp, close_x - 2, close_y + 2, 0xFF1E1E2E);

        /* Minimize button — yellow circle with − glyph */
        int min_x = wx + ww - 38;
        int min_y = wy - AZWM_TITLEBAR_H + 12;
        bb_fill_circle(comp, min_x, min_y, 7, 0xFFF9E2AF);
        bb_fill_rect(comp, min_x - 3, min_y, 7, 2, 0xFF1E1E2E);

        /* Maximize button — green circle with □ glyph */
        int max_x = wx + ww - 58;
        int max_y = wy - AZWM_TITLEBAR_H + 12;
        bb_fill_circle(comp, max_x, max_y, 7, 0xFFA6E3A1);
        bb_fill_rect(comp, max_x - 3, max_y - 3, 7, 1, 0xFF1E1E2E);
        bb_fill_rect(comp, max_x - 3, max_y + 3, 7, 1, 0xFF1E1E2E);
        bb_fill_rect(comp, max_x - 3, max_y - 2, 1, 5, 0xFF1E1E2E);
    }

    if (win->pixels && (unsigned long)win->pixels >= 0x40000000UL) {
        unsigned int pitch_px = comp->fb_pitch / 4;
        int max_rows = wh;
        if (win->buffer_h > 0 && max_rows > (int)win->buffer_h)
            max_rows = (int)win->buffer_h;

        for (int row = 0; row < max_rows; row++) {
            int sy = wy + row;
            if (sy < 0 || sy >= (int)comp->fb_height) continue;

            int src_x = 0;
            int dst_x = wx;
            int copy_w = ww;

            if (win->buffer_w > 0 && copy_w > (int)win->buffer_w)
                copy_w = (int)win->buffer_w;

            if (dst_x < 0) {
                src_x = -dst_x;
                copy_w += dst_x;
                dst_x = 0;
            }
            if (dst_x + copy_w > (int)comp->fb_width) {
                copy_w = (int)comp->fb_width - dst_x;
            }
            if (win->buffer_w > 0 && src_x + copy_w > (int)win->buffer_w) {
                copy_w = (int)win->buffer_w - src_x;
            }

            if (copy_w > 0 && win->buffer_w > 0) {
                unsigned long offset = (unsigned long)row * win->buffer_w + (unsigned long)src_x;
                /* SHM Bounds validation: never read beyond allocated buffer */
                if (win->shm_bytes == 0 || (offset + copy_w) * sizeof(unsigned int) <= win->shm_bytes) {
                    unsigned int *src_ptr = &win->pixels[offset];
                    unsigned int *dst_ptr = &comp->backbuf[(unsigned int)sy * pitch_px + (unsigned int)dst_x];

                    for (int px = 0; px < copy_w; px++) {
                        unsigned int col = src_ptr[px];
                        unsigned int a = (col >> 24) & 0xFF;
                        if (a == 255) {
                            dst_ptr[px] = col;
                        } else if (a > 0) {
                            dst_ptr[px] = alpha_blend(dst_ptr[px], col, a);
                        }
                    }
                }
            }
        }
    }
    /* Render resize grip in bottom-right corner of framed windows */
    if (has_frame && !win->maximized) {
        int rx = wx + ww - 10;
        int ry = wy + wh - 10;
        unsigned int grip_col = win->focused ? 0xFF89B4FA : 0xFF585B70;
        bb_put_pixel(comp, rx + 6, ry + 6, grip_col);
        bb_put_pixel(comp, rx + 4, ry + 6, grip_col);
        bb_put_pixel(comp, rx + 6, ry + 4, grip_col);
        bb_put_pixel(comp, rx + 2, ry + 6, grip_col);
        bb_put_pixel(comp, rx + 4, ry + 4, grip_col);
        bb_put_pixel(comp, rx + 6, ry + 2, grip_col);
    }
}

static void draw_snap_preview(az_compositor_t *comp)
{
    if (comp->snap_preview_mode == 0) return;

    int sx = 0, sy = 0, sw = 0, sh = 0;
    int fb_w = (int)comp->fb_width;
    int fb_h = (int)comp->fb_height;

    if (comp->snap_preview_mode == 1) {
        /* Left half snap */
        sx = AZWM_BORDER_W;
        sy = AZWM_TITLEBAR_H + AZWM_BORDER_W;
        sw = (fb_w / 2) - 2 * AZWM_BORDER_W;
        sh = fb_h - 40 - AZWM_TITLEBAR_H - 2 * AZWM_BORDER_W;
    } else if (comp->snap_preview_mode == 2) {
        /* Right half snap */
        sx = (fb_w / 2) + AZWM_BORDER_W;
        sy = AZWM_TITLEBAR_H + AZWM_BORDER_W;
        sw = (fb_w / 2) - 2 * AZWM_BORDER_W;
        sh = fb_h - 40 - AZWM_TITLEBAR_H - 2 * AZWM_BORDER_W;
    } else if (comp->snap_preview_mode == 3) {
        /* Maximize snap */
        sx = AZWM_BORDER_W;
        sy = AZWM_TITLEBAR_H + AZWM_BORDER_W;
        sw = fb_w - 2 * AZWM_BORDER_W;
        sh = fb_h - 40 - AZWM_TITLEBAR_H - 2 * AZWM_BORDER_W;
    } else if (comp->snap_preview_mode == 4) {
        /* Top-Left quarter snap */
        sx = AZWM_BORDER_W;
        sy = AZWM_TITLEBAR_H + AZWM_BORDER_W;
        sw = (fb_w / 2) - 2 * AZWM_BORDER_W;
        sh = (fb_h - 40) / 2 - AZWM_TITLEBAR_H - 2 * AZWM_BORDER_W;
    } else if (comp->snap_preview_mode == 5) {
        /* Top-Right quarter snap */
        sx = (fb_w / 2) + AZWM_BORDER_W;
        sy = AZWM_TITLEBAR_H + AZWM_BORDER_W;
        sw = (fb_w / 2) - 2 * AZWM_BORDER_W;
        sh = (fb_h - 40) / 2 - AZWM_TITLEBAR_H - 2 * AZWM_BORDER_W;
    } else if (comp->snap_preview_mode == 6) {
        /* Bottom-Left quarter snap */
        sx = AZWM_BORDER_W;
        sy = (fb_h - 40) / 2 + AZWM_BORDER_W;
        sw = (fb_w / 2) - 2 * AZWM_BORDER_W;
        sh = (fb_h - 40) / 2 - 2 * AZWM_BORDER_W;
    } else if (comp->snap_preview_mode == 7) {
        /* Bottom-Right quarter snap */
        sx = (fb_w / 2) + AZWM_BORDER_W;
        sy = (fb_h - 40) / 2 + AZWM_BORDER_W;
        sw = (fb_w / 2) - 2 * AZWM_BORDER_W;
        sh = (fb_h - 40) / 2 - 2 * AZWM_BORDER_W;
    }

    if (sw <= 0 || sh <= 0) return;

    unsigned int pitch_px = comp->fb_pitch / 4;
    /* Translucent frosted glass tint (0x4589B4FA) */
    for (int y = sy; y < sy + sh; y++) {
        if (y < 0 || y >= fb_h) continue;
        unsigned int *line = &comp->backbuf[y * pitch_px];
        for (int x = sx; x < sx + sw; x++) {
            if (x < 0 || x >= fb_w) continue;
            line[x] = alpha_blend(line[x], 0xFF89B4FA, 68);
        }
    }

    /* Glowing rounded border */
    for (int x = sx; x < sx + sw; x++) {
        if (x >= 0 && x < fb_w) {
            if (sy >= 0 && sy < fb_h) comp->backbuf[sy * pitch_px + x] = 0xFF89B4FA;
            if (sy + 1 >= 0 && sy + 1 < fb_h) comp->backbuf[(sy + 1) * pitch_px + x] = 0xFFB4BEFE;
            if (sy + sh - 1 >= 0 && sy + sh - 1 < fb_h) comp->backbuf[(sy + sh - 1) * pitch_px + x] = 0xFF89B4FA;
            if (sy + sh - 2 >= 0 && sy + sh - 2 < fb_h) comp->backbuf[(sy + sh - 2) * pitch_px + x] = 0xFFB4BEFE;
        }
    }
    for (int y = sy; y < sy + sh; y++) {
        if (y >= 0 && y < fb_h) {
            if (sx >= 0 && sx < fb_w) comp->backbuf[y * pitch_px + sx] = 0xFF89B4FA;
            if (sx + 1 >= 0 && sx + 1 < fb_w) comp->backbuf[y * pitch_px + sx + 1] = 0xFFB4BEFE;
            if (sx + sw - 1 >= 0 && sx + sw - 1 < fb_w) comp->backbuf[y * pitch_px + sx + sw - 1] = 0xFF89B4FA;
            if (sx + sw - 2 >= 0 && sx + sw - 2 < fb_w) comp->backbuf[y * pitch_px + sx + sw - 2] = 0xFFB4BEFE;
        }
    }
}

static void draw_alt_tab_hud(az_compositor_t *comp)
{
    if (!comp->alt_tab_active || comp->alt_tab_count <= 0) return;

    int hud_w = 460;
    int hud_h = 56 + comp->alt_tab_count * 34;
    int hx = ((int)comp->fb_width - hud_w) / 2;
    int hy = ((int)comp->fb_height - hud_h) / 2;
    if (hx < 0) hx = 0;
    if (hy < 0) hy = 0;

    /* Drop shadow for HUD */
    draw_drop_shadow(comp, hx, hy, hud_w, hud_h);

    /* Background panel with rounded rect */
    bb_fill_rounded_rect(comp, hx, hy, hud_w, hud_h, 12, 0xFF181825);
    /* Border outline */
    bb_fill_rounded_rect(comp, hx, hy, hud_w, 2, 0, 0xFFB4BEFE);

    /* Header title */
    const char *hdr = "Active Applications (Alt + Tab)";
    for (int i = 0; hdr[i]; i++) {
        desktop_draw_char_at(comp->backbuf, comp->fb_width, comp->fb_height,
                             comp->fb_pitch / 4, hx + 18 + i * 8, hy + 14, hdr[i], 0xFFCBA6F7);
    }

    /* Separator line */
    unsigned int pitch_px = comp->fb_pitch / 4;
    int sep_y = hy + 38;
    if (sep_y >= 0 && sep_y < (int)comp->fb_height) {
        for (int x = hx + 16; x < hx + hud_w - 16; x++) {
            if (x >= 0 && x < (int)comp->fb_width) comp->backbuf[sep_y * pitch_px + x] = 0xFF313244;
        }
    }

    /* List windows */
    for (int i = 0; i < comp->alt_tab_count; i++) {
        unsigned int wid = comp->alt_tab_wids[i];
        az_window_t *win = 0;
        for (int k = 0; k < AZWM_MAX_WINDOWS; k++) {
            if (comp->window_pool[k].wid == wid) {
                win = &comp->window_pool[k];
                break;
            }
        }
        if (!win) continue;

        int iy = hy + 46 + i * 34;
        bool is_sel = (i == comp->alt_tab_idx);

        if (is_sel) {
            bb_fill_rounded_rect(comp, hx + 12, iy, hud_w - 24, 28, 6, 0xFF313244);
            /* Selected left accent indicator */
            bb_fill_rounded_rect(comp, hx + 14, iy + 4, 4, 20, 2, 0xFF89B4FA);
        }

        /* Bullet / app icon dot */
        bb_fill_circle(comp, hx + 28, iy + 14, 4, is_sel ? 0xFFA6E3A1 : 0xFF585B70);

        /* Window title text */
        const char *t = win->title[0] ? win->title : "Application Window";
        int tx = hx + 40;
        for (int c = 0; t[c] && tx < hx + hud_w - 30; c++) {
            desktop_draw_char_at(comp->backbuf, comp->fb_width, comp->fb_height,
                                 comp->fb_pitch / 4, tx, iy + 10, t[c],
                                 is_sel ? 0xFFCDD6F4 : 0xFF9399B2);
            tx += 8;
        }
    }
}

static const char *g_ctx_menu_items[] = {
    " Terminal",
    " File Manager",
    " Text Editor",
    " Calculator",
    " Paint Studio",
    " System Monitor",
    " Settings",
    " Refresh Desktop"
};
#define CTX_MENU_COUNT (sizeof(g_ctx_menu_items) / sizeof(g_ctx_menu_items[0]))

static void draw_context_menu(az_compositor_t *comp)
{
    if (!comp->ctx_menu_active) return;

    int mx = comp->ctx_menu_x;
    int my = comp->ctx_menu_y;
    int mw = 176;
    int mh = (int)CTX_MENU_COUNT * 26 + 12;

    if (mx + mw > (int)comp->fb_width - 8) mx = (int)comp->fb_width - mw - 8;
    if (my + mh > (int)comp->fb_height - 48) my = (int)comp->fb_height - mh - 48;
    if (mx < 8) mx = 8;
    if (my < 8) my = 8;

    /* Shadow */
    draw_drop_shadow(comp, mx, my, mw, mh);

    /* Main Menu Frame with top accent */
    bb_fill_rounded_rect(comp, mx, my, mw, mh, 8, 0xFF181825);
    bb_fill_rounded_rect(comp, mx, my, mw, 2, 0, 0xFF89B4FA);

    /* Render Menu Items */
    for (size_t i = 0; i < CTX_MENU_COUNT; i++) {
        int iy = my + 6 + (int)i * 26;
        bool is_hov = ((int)i == comp->ctx_menu_hover);

        if (is_hov) {
            bb_fill_rounded_rect(comp, mx + 6, iy, mw - 12, 24, 4, 0xFF313244);
            bb_fill_rounded_rect(comp, mx + 8, iy + 4, 3, 16, 2, 0xFF89B4FA);
        }

        /* Dot indicator */
        unsigned int dot_color = is_hov ? 0xFF89B4FA : 0xFF6C7086;
        bb_fill_circle(comp, mx + 18, iy + 12, 3, dot_color);

        /* Label */
        desktop_draw_text_at(comp->backbuf, comp->fb_width, comp->fb_height,
                             comp->fb_pitch / 4, mx + 28, iy + 6,
                             g_ctx_menu_items[i],
                             is_hov ? 0xFFFFFFFF : 0xFFCDD6F4);
    }
}

static void compositor_restore_cursor_area(az_compositor_t *comp, int cx, int cy)
{
    unsigned int pitch_px = comp->fb_pitch / 4;
    int ox = cx - 2;
    int oy = cy - 2;
    int cw = AZ_WM_CURSOR_W + 4;
    int ch = AZ_WM_CURSOR_H + 4;
    
    for (int y = 0; y < ch; y++) {
        int sy = oy + y;
        if (sy < 0) continue;
        if (sy >= (int)comp->fb_height) break;
        int sx = ox;
        int w = cw;
        if (sx < 0) {
            w += sx;
            sx = 0;
        }
        if (sx + w > (int)comp->fb_width) w = (int)comp->fb_width - sx;
        if (w > 0) {
            memcpy(&comp->frontbuf[(unsigned int)sy * pitch_px + (unsigned int)sx], 
                   &comp->backbuf[(unsigned int)sy * pitch_px + (unsigned int)sx], 
                   (size_t)w * sizeof(unsigned int));
        }
    }
}

void compose_screen(az_compositor_t *comp)
{
    unsigned int pitch_px = comp->fb_pitch / 4;

    /* ── 1. Clear backbuf only if tail window does not fully cover screen ─ */
    az_window_t *tail = comp->list_tail;
    bool tail_has_frame = (tail && tail->title[0] != '\0');
    bool full_coverage = (tail && tail->visible && tail->wid != 0 && !tail_has_frame &&
                          tail->x <= 0 && tail->y <= 0 &&
                          tail->width >= comp->fb_width && tail->height >= comp->fb_height);
    if (!full_coverage) {
        bb_fill_rect(comp, 0, 0, comp->fb_width, comp->fb_height, 0xFF1E1E2E);
    }

    /* ── 2. Draw all windows (back to front in Z-order) ───────────────── */
    az_window_t *curr = comp->list_tail;
    while (curr) {
        render_window(comp, curr);
        curr = curr->prev;
    }

    /* ── 3. Draw Snapping Preview, Alt+Tab HUD & Context Menu Overlays ─── */
    draw_snap_preview(comp);
    draw_alt_tab_hud(comp);
    draw_context_menu(comp);

    /* ── 4. Presentation: Hardware Zero-Copy Page Flip or Vectorized Blit ─ */
    if (comp->hw_page_flip) {
        int next_buf = 1 - comp->active_vram_buf;
        if (az_fb_flip((unsigned int)next_buf) == 0) {
            comp->active_vram_buf = next_buf;
            comp->frontbuf = comp->vram_buf[comp->active_vram_buf];
            comp->backbuf  = comp->vram_buf[1 - comp->active_vram_buf];

            /* Draw mouse cursor directly to the newly flipped front buffer */
            desktop_draw_cursor(comp->frontbuf, comp->fb_width, comp->fb_height, pitch_px,
                                comp->cursor_x, comp->cursor_y);
            comp->old_cursor_x = comp->cursor_x;
            comp->old_cursor_y = comp->cursor_y;
            comp->has_damage = 0;
            comp->dirty_min_x = (int)comp->fb_width;
            comp->dirty_min_y = (int)comp->fb_height;
            comp->dirty_max_x = 0;
            comp->dirty_max_y = 0;
            return;
        }
    }

    /* Fallback Double-Buffering: copy damaged rects back buffer → front buffer */
    int y0 = comp->dirty_min_y < 0 ? 0 : comp->dirty_min_y;
    int y1 = comp->dirty_max_y > (int)comp->fb_height ? (int)comp->fb_height : comp->dirty_max_y;
    int x0 = comp->dirty_min_x < 0 ? 0 : comp->dirty_min_x;
    int x1 = comp->dirty_max_x > (int)comp->fb_width ? (int)comp->fb_width : comp->dirty_max_x;

    if (x0 < x1 && y0 < y1 && (x1 - x0 < (int)comp->fb_width || y1 - y0 < (int)comp->fb_height)) {
        size_t row_bytes = (size_t)(x1 - x0) * sizeof(unsigned int);
        for (int y = y0; y < y1; y++) {
            unsigned int *src_row = &comp->backbuf[y * pitch_px + x0];
            unsigned int *dst_row = &comp->frontbuf[y * pitch_px + x0];
            memcpy(dst_row, src_row, row_bytes);
        }
    } else {
        size_t total_bytes = (size_t)pitch_px * comp->fb_height * sizeof(unsigned int);
        memcpy(comp->frontbuf, comp->backbuf, total_bytes);
    }

    comp->has_damage = 0;
    comp->dirty_min_x = (int)comp->fb_width;
    comp->dirty_min_y = (int)comp->fb_height;
    comp->dirty_max_x = 0;
    comp->dirty_max_y = 0;

    /* Draw mouse cursor directly to frontbuf */
    desktop_draw_cursor(comp->frontbuf, comp->fb_width, comp->fb_height, pitch_px,
                        comp->cursor_x, comp->cursor_y);
    comp->old_cursor_x = comp->cursor_x;
    comp->old_cursor_y = comp->cursor_y;

}

void compositor_update_cursor(az_compositor_t *comp)
{
    unsigned int pitch_px = comp->fb_pitch / 4;
    
    /* 1. Restore old cursor region from backbuf to frontbuf */
    compositor_restore_cursor_area(comp, comp->old_cursor_x, comp->old_cursor_y);
    
    /* 2. Draw new cursor to frontbuf */
    desktop_draw_cursor(comp->frontbuf, comp->fb_width, comp->fb_height, pitch_px,
                        comp->cursor_x, comp->cursor_y);
    
    comp->old_cursor_x = comp->cursor_x;
    comp->old_cursor_y = comp->cursor_y;
}

/* ── Animation Engine ────────────────────────────────────────────────────── */

void compositor_trigger_open_animation(az_compositor_t *comp, az_window_t *win)
{
    if (!win || win->title[0] == '\0') return;
    win->anim_state    = AZWM_ANIM_OPEN;
    win->anim_step     = 0;
    win->anim_target_x = win->x;
    win->anim_target_y = win->y;
    win->anim_target_w = win->width;
    win->anim_target_h = win->height;

    /* Zoom in from center point */
    int cx = win->x + (int)win->width / 2;
    int cy = win->y + (int)win->height / 2;
    win->anim_start_w = win->width / 4;
    win->anim_start_h = win->height / 4;
    if (win->anim_start_w < 60) win->anim_start_w = 60;
    if (win->anim_start_h < 40) win->anim_start_h = 40;
    win->anim_start_x = cx - (int)win->anim_start_w / 2;
    win->anim_start_y = cy - (int)win->anim_start_h / 2;

    win->x = win->anim_start_x;
    win->y = win->anim_start_y;
    win->width = win->anim_start_w;
    win->height = win->anim_start_h;
    comp->has_animating_windows = 1;
}

void compositor_trigger_minimize_animation(az_compositor_t *comp, az_window_t *win, int dock_x, int dock_y)
{
    if (!win) return;
    win->anim_state    = AZWM_ANIM_MINIMIZE;
    win->anim_step     = 0;
    win->anim_start_x  = win->x;
    win->anim_start_y  = win->y;
    win->anim_start_w  = win->width;
    win->anim_start_h  = win->height;

    win->saved_x = win->x;
    win->saved_y = win->y;
    win->saved_w = win->width;
    win->saved_h = win->height;

    win->anim_target_x = dock_x;
    win->anim_target_y = dock_y;
    win->anim_target_w = 40;
    win->anim_target_h = 24;
    comp->has_animating_windows = 1;
}

void compositor_trigger_restore_animation(az_compositor_t *comp, az_window_t *win, int dock_x, int dock_y)
{
    if (!win) return;
    win->anim_state    = AZWM_ANIM_RESTORE;
    win->anim_step     = 0;
    win->visible       = 1;
    win->anim_start_x  = dock_x;
    win->anim_start_y  = dock_y;
    win->anim_start_w  = 40;
    win->anim_start_h  = 24;

    win->anim_target_x = win->saved_x > 0 ? win->saved_x : win->x;
    win->anim_target_y = win->saved_y > 0 ? win->saved_y : win->y;
    win->anim_target_w = win->saved_w > 0 ? win->saved_w : win->width;
    win->anim_target_h = win->saved_h > 0 ? win->saved_h : win->height;

    win->x = win->anim_start_x;
    win->y = win->anim_start_y;
    win->width = win->anim_start_w;
    win->height = win->anim_start_h;
    comp->has_animating_windows = 1;
}

int compositor_animate_step(az_compositor_t *comp)
{
    int still_animating = 0;
    az_window_t *curr = comp->list_head;
    while (curr) {
        if (curr->anim_state != AZWM_ANIM_NONE) {
            curr->anim_step++;
            /* Ease-out quadratic: progress = t * (512 - t) / 256 */
            int t = curr->anim_step * 256 / AZWM_ANIM_STEPS;
            if (t > 256) t = 256;
            int ease = (t * (512 - t)) / 256;

            curr->x = curr->anim_start_x + ((curr->anim_target_x - curr->anim_start_x) * ease) / 256;
            curr->y = curr->anim_start_y + ((curr->anim_target_y - curr->anim_start_y) * ease) / 256;
            curr->width = (unsigned int)((int)curr->anim_start_w + ((int)(curr->anim_target_w - curr->anim_start_w) * ease) / 256);
            curr->height = (unsigned int)((int)curr->anim_start_h + ((int)(curr->anim_target_h - curr->anim_start_h) * ease) / 256);

            if (curr->anim_step >= AZWM_ANIM_STEPS) {
                if (curr->anim_state == AZWM_ANIM_MINIMIZE) {
                    curr->visible = 0;
                    curr->x = curr->saved_x;
                    curr->y = curr->saved_y;
                    curr->width = curr->saved_w;
                    curr->height = curr->saved_h;
                } else {
                    curr->x = curr->anim_target_x;
                    curr->y = curr->anim_target_y;
                    curr->width = curr->anim_target_w;
                    curr->height = curr->anim_target_h;
                }
                curr->anim_state = AZWM_ANIM_NONE;
                curr->anim_step = 0;
            } else {
                still_animating = 1;
            }
        }
        curr = curr->next;
    }
    comp->has_animating_windows = still_animating;
    return still_animating;
}

