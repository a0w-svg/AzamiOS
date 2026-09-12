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
#include "../../libc/include/time.h"
#include <stdbool.h>
#include <emmintrin.h>
#include <immintrin.h>

/* Runtime AVX2 gate — see the identical check in shared/gfx_pipeline.h for
 * why this needs XGETBV and not just the CPUID feature bit: the binary is
 * built for the SSE2-only baseline (no -march here), so the AVX2 codepaths
 * below only run from behind __attribute__((target("avx2"))) functions,
 * never because the whole translation unit was compiled for AVX2. */
static inline int compositor_cpu_has_avx2(void)
{
    static int cached = -1;
    if (cached >= 0) return cached;

    unsigned int eax, ebx, ecx, edx;
    __asm__ __volatile__("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                                 : "a"(1), "c"(0));
    if (!(ecx & (1u << 27)) || !(ecx & (1u << 28))) { cached = 0; return 0; }

    unsigned int xcr0_lo, xcr0_hi;
    __asm__ __volatile__("xgetbv" : "=a"(xcr0_lo), "=d"(xcr0_hi) : "c"(0));
    if ((xcr0_lo & 0x6u) != 0x6u) { cached = 0; return 0; }

    __asm__ __volatile__("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                                 : "a"(7), "c"(0));
    cached = (ebx & (1u << 5)) ? 1 : 0;
    return cached;
}

/* ── Pixel buffer base address for shared memory window buffers ───────────── */
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/fb.h>

#define SHMEM_WINDOW_BASE  0x50000000UL
#define SHMEM_WINDOW_STEP  0x01000000UL  /* 16 MB per window */

static inline bool win_has_frame(const az_window_t *win)
{
    if (!win || win->title[0] == '\0') return false;
    if (strcmp(win->title, "AzamiOS App Launcher") == 0) return false;
    return true;
}

/*
 * monotonic_now_ns() — CLOCK_MONOTONIC in nanoseconds, or 0 on failure (the
 * kernel is expected to always support this clock; the fallback just makes
 * a transient failure a no-op frame instead of reading garbage from an
 * uninitialized timespec). Shared by the FPS counter in
 * compositor_present_internal() and the window animation timing below —
 * both want "how much wall-clock time actually passed", not a frame count,
 * for the same reason: neither should depend on how fast this process
 * happens to be looping.
 */
static inline long long monotonic_now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (long long)ts.tv_sec * 1000000000LL + (long long)ts.tv_nsec;
}

long long compositor_now_ns(void)
{
    return monotonic_now_ns();
}

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
    comp->current_cursor_type = AZ_CURSOR_DEFAULT;
    comp->hover_btn_wid  = 0;
    comp->hover_btn      = AZWM_BTN_NONE;
    comp->server_channel = server_channel;
    comp->has_animating_windows = 0;

    /* Setup hardware double-buffered VRAM pointers */
    comp->frontbuf = frontbuf;
    comp->backbuf  = backbuf;
    comp->hw_page_flip = 0;
    comp->fb_fd        = -1;
    comp->fb_yres      = h;
    comp->vram_buf[0]  = frontbuf;
    comp->vram_buf[1]  = frontbuf;
    comp->active_vram_buf = 0;
    comp->pending[0].valid = 0;
    comp->pending[1].valid = 0;
    comp->cursor_rect[0].valid = 0;
    comp->cursor_rect[1].valid = 0;
    comp->frame_count          = 0;
    comp->current_fps          = 60;
    comp->last_fps_time        = 0;
    comp->last_fps_frame_count = 0;
    comp->fps_hud_visible      = 0;

    comp->list_head = 0; /* NULL */
    comp->list_tail = 0; /* NULL */
    
    /* Initialize memory pool and string them into free_list */
    comp->free_list = 0; /* NULL */
    for (int i = AZWM_MAX_WINDOWS - 1; i >= 0; i--) {
        comp->window_pool[i].wid = 0;
        comp->window_pool[i].opacity = 255;
        comp->window_pool[i].pinned = 0;
        comp->window_pool[i].cursor_type = AZ_CURSOR_DEFAULT;
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
                             unsigned int flags,
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
    win->opacity     = 255;
    win->pinned      = 0;
    win->cursor_type = AZ_CURSOR_DEFAULT;
    win->blur_backdrop = (flags & AZ_WIN_FLAG_BLUR_BACKDROP) ? 1 : 0;
    win->anim_state  = AZWM_ANIM_NONE;
    win->anim_start_ns = 0;

    int ti = 0;
    if (title) {
        while (title[ti] && ti < 63) { win->title[ti] = title[ti]; ti++; }
    }
    win->title[ti] = '\0';

    unsigned long pixels_total = (unsigned long)w * h;
    unsigned long pi = 0;
    __m128i bg128 = _mm_set1_epi32((int)0xFF1E1E2EU);
    for (; pi + 4 <= pixels_total; pi += 4) {
        _mm_storeu_si128((__m128i *)(win->pixels + pi), bg128);
    }
    for (; pi < pixels_total; pi++) {
        win->pixels[pi] = 0xFF1E1E2EU; 
    }

    /* Add to Z-order front */
    list_push_front(comp, win);
    comp->window_count++;

    if (out_shmem_id) *out_shmem_id = (unsigned int)shmem_id;

    compositor_focus_window(comp, win);
    if (win_has_frame(win)) {
        compositor_trigger_open_animation(comp, win);
    }
    return (int)win->wid;
}

/* ── Window Resize ────────────────────────────────────────────────────────── */

/*
 * Reallocate a window's content surface for a new size.
 *
 * A drag-resize, edge-snap, or maximize toggle used to just overwrite
 * win->width/height for chrome and hit-testing while the shmem surface
 * compositor_create_window() allocated stayed sized for whatever the window
 * was created at. render_window() clips its client-area blit to
 * buffer_w/buffer_h, so azwm itself never over-read that stale surface —
 * but the *client* trusts width == the surface it was handed, and a client
 * that ever redraws at its own (unreloaded) idea of a bigger size, or is
 * simply told a wrong size, is one dropped notification away from writing
 * past a too-small mapping. This closes that gap: the surface is
 * reallocated up front and the new shmem_id handed back so the caller can
 * push it to the client, instead of geometry drifting from the surface
 * silently.
 *
 * Returns 0 and fills *out_shmem_id on success — a no-op returning the
 * window's existing shmem_id when the clamped target already matches
 * buffer_w/buffer_h, so callers can call this unconditionally without
 * checking first. Returns -1 on allocation failure, in which case the
 * window is left with no valid surface (pixels/shmem_id/buffer_w/buffer_h
 * all zeroed) and the caller must destroy it — there is nothing left to
 * safely composite or hand to the client.
 */
int compositor_resize_window(az_compositor_t *comp, az_window_t *win,
                             unsigned int new_w, unsigned int new_h,
                             unsigned int *out_shmem_id)
{
    if (!win || win->wid == 0) return -1;

    if (new_w < 100) new_w = 100;
    if (new_h < 60)  new_h = 60;
    if (new_w > comp->fb_width)  new_w = comp->fb_width;
    if (new_h > comp->fb_height) new_h = comp->fb_height;

    if (new_w == win->buffer_w && new_h == win->buffer_h) {
        if (out_shmem_id) *out_shmem_id = win->shmem_id;
        return 0;
    }

    unsigned long buf_size   = (unsigned long)new_w * new_h * 4;
    unsigned long page_count = (buf_size + 4095) / 4096;
    if (page_count == 0) page_count = 1;

    int new_shmem_id = az_shmem_create((int)page_count);
    if (new_shmem_id < 0) return -1;

    /* Same per-slot VA every window's surface has always lived at — up to
     * SHMEM_WINDOW_STEP (16 MB) of headroom, far more than fb_width *
     * fb_height * 4 ever needs even at full-screen, so remapping in place
     * can't collide with a neighbouring window's slot. */
    int slot = (int)(win - comp->window_pool);
    unsigned long map_addr = SHMEM_WINDOW_BASE + (unsigned long)slot * SHMEM_WINDOW_STEP;

    az_shmem_unmap((int)win->shmem_id, (void *)map_addr);
    az_shmem_destroy((int)win->shmem_id);

    if (az_shmem_map(new_shmem_id, (void *)map_addr) < 0) {
        az_shmem_destroy(new_shmem_id);
        win->pixels    = 0;
        win->shmem_id  = 0;
        win->shm_bytes = 0;
        win->buffer_w  = 0;
        win->buffer_h  = 0;
        return -1;
    }

    win->pixels    = (unsigned int *)map_addr;
    win->shmem_id  = (unsigned int)new_shmem_id;
    win->shm_bytes = page_count * 4096;
    win->buffer_w  = new_w;
    win->buffer_h  = new_h;

    /* Fresh shmem pages come back zeroed (transparent black), and the
     * client's next frame may only cover part of the new surface (partial
     * damage) — paint the same idle background compositor_create_window()
     * gives a brand-new window so a not-yet-redrawn area reads as an empty
     * window instead of a transparent hole into whatever is behind it. */
    unsigned long pixels_total = (unsigned long)new_w * new_h;
    unsigned long pi = 0;
    __m128i bg128 = _mm_set1_epi32((int)0xFF1E1E2EU);
    for (; pi + 4 <= pixels_total; pi += 4)
        _mm_storeu_si128((__m128i *)(win->pixels + pi), bg128);
    for (; pi < pixels_total; pi++)
        win->pixels[pi] = 0xFF1E1E2EU;

    if (out_shmem_id) *out_shmem_id = (unsigned int)new_shmem_id;
    return 0;
}

/* ── Window Destruction ──────────────────────────────────────────────────── */

void compositor_destroy_window(az_compositor_t *comp, unsigned int wid)
{
    az_window_t *curr = comp->list_head;
    while (curr) {
        if (curr->wid == wid) {
            int dmg_x = curr->x - AZWM_BORDER_W - 16;
            int dmg_y = curr->y - (AZWM_TITLEBAR_H + AZWM_BORDER_W) - 16;
            int dmg_w = (int)curr->width + 2 * AZWM_BORDER_W + 32;
            int dmg_h = (int)curr->height + AZWM_TITLEBAR_H + 2 * AZWM_BORDER_W + 32;
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

        bool has_frame = win_has_frame(curr);
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
    if (comp->focused_window == win) return;

    if (comp->focused_window) {
        az_window_t *prev = comp->focused_window;
        prev->focused = 0;
        compositor_damage(comp, prev->x - AZWM_BORDER_W - 16,
                          prev->y - (AZWM_TITLEBAR_H + AZWM_BORDER_W) - 16,
                          (int)prev->width + 2 * AZWM_BORDER_W + 32,
                          (int)prev->height + AZWM_TITLEBAR_H + 2 * AZWM_BORDER_W + 32);
    }
    comp->focused_window = win;
    if (win) {
        win->focused = 1;
        /* Move to top of Z-order */
        if (comp->list_head != win) {
            list_remove(comp, win);
            list_push_front(comp, win);
        }
        compositor_damage(comp, win->x - AZWM_BORDER_W - 16,
                          win->y - (AZWM_TITLEBAR_H + AZWM_BORDER_W) - 16,
                          (int)win->width + 2 * AZWM_BORDER_W + 32,
                          (int)win->height + AZWM_TITLEBAR_H + 2 * AZWM_BORDER_W + 32);
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

/* One 4-pixel step of alpha_shade_black_span's translucent case. Factored out
 * so the AVX2 path below can call the exact same math on its own remainder
 * instead of re-deriving it — the two must never be free to disagree. */
static inline void alpha_shade_black_block4_sse2(unsigned int *dst, __m128i inv_a_vec, __m128i alpha_mask)
{
    __m128i zero = _mm_setzero_si128();
    __m128i d = _mm_loadu_si128((const __m128i *)dst);
    __m128i d_lo = _mm_unpacklo_epi8(d, zero);
    __m128i d_hi = _mm_unpackhi_epi8(d, zero);

    __m128i res_lo = _mm_srli_epi16(_mm_mullo_epi16(d_lo, inv_a_vec), 8);
    __m128i res_hi = _mm_srli_epi16(_mm_mullo_epi16(d_hi, inv_a_vec), 8);

    __m128i res = _mm_packus_epi16(res_lo, res_hi);
    res = _mm_or_si128(res, alpha_mask);
    _mm_storeu_si128((__m128i *)dst, res);
}

__attribute__((target("avx2")))
static void alpha_shade_black_span_avx2(unsigned int *dst, unsigned int alpha, int count)
{
    unsigned int inv_a = 255 - alpha;
    __m256i inv_a_vec = _mm256_set1_epi16((short)inv_a);
    __m256i zero = _mm256_setzero_si256();
    __m256i alpha_mask = _mm256_set1_epi32((int)0xFF000000U);

    int px = 0;
    for (; px + 8 <= count; px += 8) {
        __m256i d = _mm256_loadu_si256((const __m256i *)(dst + px));
        __m256i d_lo = _mm256_unpacklo_epi8(d, zero);
        __m256i d_hi = _mm256_unpackhi_epi8(d, zero);

        __m256i res_lo = _mm256_srli_epi16(_mm256_mullo_epi16(d_lo, inv_a_vec), 8);
        __m256i res_hi = _mm256_srli_epi16(_mm256_mullo_epi16(d_hi, inv_a_vec), 8);

        __m256i res = _mm256_packus_epi16(res_lo, res_hi);
        res = _mm256_or_si256(res, alpha_mask);
        _mm256_storeu_si256((__m256i *)(dst + px), res);
    }
    if (px + 4 <= count) {
        __m128i inv_a_vec4 = _mm_set1_epi16((short)inv_a);
        __m128i alpha_mask4 = _mm_set1_epi32((int)0xFF000000U);
        alpha_shade_black_block4_sse2(dst + px, inv_a_vec4, alpha_mask4);
        px += 4;
    }
    for (; px < count; px++) dst[px] = alpha_shade_black(dst[px], alpha);
}

__attribute__((target("avx2")))
static void alpha_shade_black_span_fill_avx2(unsigned int *dst, int count)
{
    __m256i black = _mm256_set1_epi32((int)0xFF000000U);
    int px = 0;
    for (; px + 8 <= count; px += 8) {
        _mm256_storeu_si256((__m256i *)(dst + px), black);
    }
    __m128i black4 = _mm_set1_epi32((int)0xFF000000U);
    for (; px + 4 <= count; px += 4) {
        _mm_storeu_si128((__m128i *)(dst + px), black4);
    }
    for (; px < count; px++) dst[px] = 0xFF000000U;
}

static inline void alpha_shade_black_span(unsigned int *dst, unsigned int alpha, int count)
{
    if (count <= 0 || alpha == 0) return;
    if (alpha >= 255) {
        if (compositor_cpu_has_avx2()) {
            alpha_shade_black_span_fill_avx2(dst, count);
            return;
        }
        int px = 0;
        __m128i black4 = _mm_set1_epi32((int)0xFF000000U);
        for (; px + 4 <= count; px += 4) {
            _mm_storeu_si128((__m128i *)(dst + px), black4);
        }
        for (; px < count; px++) dst[px] = 0xFF000000U;
        return;
    }

    if (compositor_cpu_has_avx2()) {
        alpha_shade_black_span_avx2(dst, alpha, count);
        return;
    }

    int px = 0;
    unsigned int inv_a = 255 - alpha;
    __m128i inv_a_vec = _mm_set1_epi16((short)inv_a);
    __m128i alpha_mask = _mm_set1_epi32((int)0xFF000000U);

    for (; px + 4 <= count; px += 4) {
        alpha_shade_black_block4_sse2(dst + px, inv_a_vec, alpha_mask);
    }
    for (; px < count; px++) {
        dst[px] = alpha_shade_black(dst[px], alpha);
    }
}

/* One 4-pixel step of composite_blend_span, opaque/transparent shortcuts
 * included — the opaque shortcut is load-bearing for correctness, not just
 * speed: the fixed-point blend below does not reproduce an opaque source
 * exactly (its rounding is one off from identity at full alpha), so skipping
 * it for a "remainder" block would make that block disagree with a same-sized
 * span processed entirely by this function. Shared by the SSE2 loop and the
 * AVX2 remainder so the two can never drift apart on it independently. */
static inline void composite_blend_block4_sse2(unsigned int *dst, const unsigned int *src)
{
    __m128i alpha_mask = _mm_set1_epi32((int)0xFF000000U);
    __m128i s = _mm_loadu_si128((const __m128i *)src);
    __m128i s_alpha = _mm_and_si128(s, alpha_mask);
    __m128i is_opaque = _mm_cmpeq_epi32(s_alpha, alpha_mask);
    if (_mm_movemask_epi8(is_opaque) == 0xFFFF) {
        _mm_storeu_si128((__m128i *)dst, s);
        return;
    }

    __m128i is_zero = _mm_cmpeq_epi32(s_alpha, _mm_setzero_si128());
    if (_mm_movemask_epi8(is_zero) == 0xFFFF) {
        return;
    }

    __m128i d = _mm_loadu_si128((const __m128i *)dst);
    __m128i zero = _mm_setzero_si128();

    __m128i s_lo = _mm_unpacklo_epi8(s, zero);
    __m128i d_lo = _mm_unpacklo_epi8(d, zero);
    __m128i a0 = _mm_shufflelo_epi16(s_lo, _MM_SHUFFLE(3, 3, 3, 3));
    a0 = _mm_shufflehi_epi16(a0, _MM_SHUFFLE(3, 3, 3, 3));
    __m128i inv_a0 = _mm_sub_epi16(_mm_set1_epi16(255), a0);
    __m128i out_lo = _mm_add_epi16(_mm_mullo_epi16(s_lo, a0), _mm_mullo_epi16(d_lo, inv_a0));
    out_lo = _mm_srli_epi16(out_lo, 8);

    __m128i s_hi = _mm_unpackhi_epi8(s, zero);
    __m128i d_hi = _mm_unpackhi_epi8(d, zero);
    __m128i a1 = _mm_shufflelo_epi16(s_hi, _MM_SHUFFLE(3, 3, 3, 3));
    a1 = _mm_shufflehi_epi16(a1, _MM_SHUFFLE(3, 3, 3, 3));
    __m128i inv_a1 = _mm_sub_epi16(_mm_set1_epi16(255), a1);
    __m128i out_hi = _mm_add_epi16(_mm_mullo_epi16(s_hi, a1), _mm_mullo_epi16(d_hi, inv_a1));
    out_hi = _mm_srli_epi16(out_hi, 8);

    __m128i out = _mm_packus_epi16(out_lo, out_hi);
    out = _mm_or_si128(out, alpha_mask);
    _mm_storeu_si128((__m128i *)dst, out);
}

__attribute__((target("avx2")))
static void composite_blend_span_avx2(unsigned int *dst, const unsigned int *src, int count)
{
    __m256i alpha_mask = _mm256_set1_epi32((int)0xFF000000U);
    __m256i zero = _mm256_setzero_si256();

    int px = 0;
    for (; px + 8 <= count; px += 8) {
        __m256i s = _mm256_loadu_si256((const __m256i *)(src + px));
        __m256i s_alpha = _mm256_and_si256(s, alpha_mask);
        __m256i is_opaque = _mm256_cmpeq_epi32(s_alpha, alpha_mask);
        if ((unsigned)_mm256_movemask_epi8(is_opaque) == 0xFFFFFFFFu) {
            _mm256_storeu_si256((__m256i *)(dst + px), s);
            continue;
        }

        __m256i is_zero = _mm256_cmpeq_epi32(s_alpha, zero);
        if ((unsigned)_mm256_movemask_epi8(is_zero) == 0xFFFFFFFFu) {
            continue;
        }

        __m256i d = _mm256_loadu_si256((const __m256i *)(dst + px));

        __m256i s_lo = _mm256_unpacklo_epi8(s, zero);
        __m256i d_lo = _mm256_unpacklo_epi8(d, zero);
        __m256i a0 = _mm256_shufflelo_epi16(s_lo, _MM_SHUFFLE(3, 3, 3, 3));
        a0 = _mm256_shufflehi_epi16(a0, _MM_SHUFFLE(3, 3, 3, 3));
        __m256i inv_a0 = _mm256_sub_epi16(_mm256_set1_epi16(255), a0);
        __m256i out_lo = _mm256_add_epi16(_mm256_mullo_epi16(s_lo, a0), _mm256_mullo_epi16(d_lo, inv_a0));
        out_lo = _mm256_srli_epi16(out_lo, 8);

        __m256i s_hi = _mm256_unpackhi_epi8(s, zero);
        __m256i d_hi = _mm256_unpackhi_epi8(d, zero);
        __m256i a1 = _mm256_shufflelo_epi16(s_hi, _MM_SHUFFLE(3, 3, 3, 3));
        a1 = _mm256_shufflehi_epi16(a1, _MM_SHUFFLE(3, 3, 3, 3));
        __m256i inv_a1 = _mm256_sub_epi16(_mm256_set1_epi16(255), a1);
        __m256i out_hi = _mm256_add_epi16(_mm256_mullo_epi16(s_hi, a1), _mm256_mullo_epi16(d_hi, inv_a1));
        out_hi = _mm256_srli_epi16(out_hi, 8);

        __m256i out = _mm256_packus_epi16(out_lo, out_hi);
        out = _mm256_or_si256(out, alpha_mask);
        _mm256_storeu_si256((__m256i *)(dst + px), out);
    }
    if (px + 4 <= count) {
        composite_blend_block4_sse2(dst + px, src + px);
        px += 4;
    }
    while (px < count) {
        unsigned int col = src[px];
        unsigned int a = (col >> 24) & 0xFF;
        if (a == 255) {
            dst[px] = col;
        } else if (a > 0) {
            dst[px] = alpha_blend(dst[px], col, a);
        }
        px++;
    }
}

/* ── SIMD / SSE2 or AVX2-accelerated parallel alpha blend span ───────────── */
static inline void composite_blend_span(unsigned int *dst, const unsigned int *src, int count)
{
    if (compositor_cpu_has_avx2()) {
        composite_blend_span_avx2(dst, src, count);
        return;
    }

    int px = 0;

    /* Process 4 pixels at a time with SSE2 */
    for (; px + 4 <= count; px += 4) {
        composite_blend_block4_sse2(dst + px, src + px);
    }

    /* Scalar remainder */
    while (px < count) {
        unsigned int col = src[px];
        unsigned int a = (col >> 24) & 0xFF;
        if (a == 255) {
            dst[px] = col;
        } else if (a > 0) {
            dst[px] = alpha_blend(dst[px], col, a);
        }
        px++;
    }
}

/* ── Parallel alpha blend span with window-level opacity ─────────────────── */
static inline void composite_blend_span_opacity(unsigned int *dst, const unsigned int *src, int count, unsigned int win_opacity)
{
    if (win_opacity >= 255) {
        composite_blend_span(dst, src, count);
        return;
    }
    if (win_opacity == 0) return;

    for (int px = 0; px < count; px++) {
        unsigned int col = src[px];
        unsigned int a = (col >> 24) & 0xFF;
        if (a > 0) {
            unsigned int eff_a = (a * win_opacity) / 255;
            if (eff_a > 0) {
                dst[px] = alpha_blend(dst[px], col, eff_a);
            }
        }
    }
}

__attribute__((target("avx2")))
static void bb_fill_span_avx2(unsigned int *dst, unsigned int color, int count)
{
    __m256i c256 = _mm256_set1_epi32((int)color);
    int i = 0;
    for (; i + 8 <= count; i += 8) {
        _mm256_storeu_si256((__m256i *)(dst + i), c256);
    }
    if (i + 4 <= count) {
        _mm_storeu_si128((__m128i *)(dst + i), _mm256_castsi256_si128(c256));
        i += 4;
    }
    while (i < count) {
        dst[i] = color;
        i++;
    }
}

static inline void bb_fill_span(unsigned int *dst, unsigned int color, int count)
{
    if (count <= 0) return;
    if (compositor_cpu_has_avx2()) {
        bb_fill_span_avx2(dst, color, count);
        return;
    }
    int i = 0;
    __m128i col128 = _mm_set1_epi32((int)color);
    for (; i + 4 <= count; i += 4) {
        _mm_storeu_si128((__m128i *)(dst + i), col128);
    }
    while (i < count) {
        dst[i] = color;
        i++;
    }
}

static void bb_fill_rect_clipped(az_compositor_t *comp, int rx, int ry, int rw, int rh, unsigned int color,
                                 int cx0, int cy0, int cx1, int cy1)
{
    if (rw <= 0 || rh <= 0) return;
    int x0 = rx < cx0 ? cx0 : rx;
    int y0 = ry < cy0 ? cy0 : ry;
    int x1 = rx + rw;
    int y1 = ry + rh;
    if (x1 > cx1) x1 = cx1;
    if (y1 > cy1) y1 = cy1;
    if (x0 >= x1 || y0 >= y1) return;

    unsigned int pitch_px = comp->fb_pitch / 4;
    int fill_w = x1 - x0;

    for (int y = y0; y < y1; y++) {
        unsigned int *dst = &comp->backbuf[(unsigned int)y * pitch_px + (unsigned int)x0];
        bb_fill_span(dst, color, fill_w);
    }
}

static void bb_fill_rect(az_compositor_t *comp, int rx, int ry, int rw, int rh, unsigned int color)
{
    bb_fill_rect_clipped(comp, rx, ry, rw, rh, color, 0, 0, (int)comp->fb_width, (int)comp->fb_height);
}

static inline void bb_put_pixel_clipped(az_compositor_t *comp, int x, int y, unsigned int color,
                                        int cx0, int cy0, int cx1, int cy1)
{
    if (x >= cx0 && x < cx1 && y >= cy0 && y < cy1 &&
        x >= 0 && x < (int)comp->fb_width && y >= 0 && y < (int)comp->fb_height) {
        unsigned int pitch_px = comp->fb_pitch / 4;
        comp->backbuf[(unsigned int)y * pitch_px + (unsigned int)x] = color;
    }
}

/* High-speed scanline span circle rasterizer with clipping */
static void bb_fill_circle_clipped(az_compositor_t *comp, int cx, int cy, int r, unsigned int color,
                                   int cx0, int cy0, int cx1, int cy1)
{
    if (r <= 0) return;
    int r2 = r * r;
    unsigned int pitch_px = comp->fb_pitch / 4;
    int fb_w = (int)comp->fb_width;
    int fb_h = (int)comp->fb_height;

    int eff_cx0 = cx0 < 0 ? 0 : cx0;
    int eff_cy0 = cy0 < 0 ? 0 : cy0;
    int eff_cx1 = cx1 > fb_w ? fb_w : cx1;
    int eff_cy1 = cy1 > fb_h ? fb_h : cy1;
    if (eff_cx0 >= eff_cx1 || eff_cy0 >= eff_cy1) return;

    for (int y = -r; y <= r; y++) {
        int py = cy + y;
        if (py < eff_cy0 || py >= eff_cy1) continue;
        int rem = r2 - y * y;
        if (rem < 0) continue;
        int max_x = 0;
        while ((max_x + 1) * (max_x + 1) <= rem) max_x++;

        int x0 = cx - max_x;
        int x1 = cx + max_x + 1;
        if (x0 < eff_cx0) x0 = eff_cx0;
        if (x1 > eff_cx1) x1 = eff_cx1;
        if (x0 >= x1) continue;

        unsigned int *dst = &comp->backbuf[(unsigned int)py * pitch_px + (unsigned int)x0];
        bb_fill_span(dst, color, x1 - x0);
    }
}

static void __attribute__((unused)) bb_fill_circle(az_compositor_t *comp, int cx, int cy, int r, unsigned int color)
{
    bb_fill_circle_clipped(comp, cx, cy, r, color, 0, 0, (int)comp->fb_width, (int)comp->fb_height);
}

/* Same fill as bb_fill_circle_clipped(), but honors @color's alpha byte via alpha_blend() */
static void bb_fill_circle_blend_clipped(az_compositor_t *comp, int cx, int cy, int r, unsigned int color,
                                         int cx0, int cy0, int cx1, int cy1)
{
    if (r <= 0) return;
    unsigned int alpha = (color >> 24) & 0xFF;
    if (alpha == 0) return;
    int r2 = r * r;
    unsigned int pitch_px = comp->fb_pitch / 4;
    int fb_w = (int)comp->fb_width;
    int fb_h = (int)comp->fb_height;

    int eff_cx0 = cx0 < 0 ? 0 : cx0;
    int eff_cy0 = cy0 < 0 ? 0 : cy0;
    int eff_cx1 = cx1 > fb_w ? fb_w : cx1;
    int eff_cy1 = cy1 > fb_h ? fb_h : cy1;
    if (eff_cx0 >= eff_cx1 || eff_cy0 >= eff_cy1) return;

    for (int y = -r; y <= r; y++) {
        int py = cy + y;
        if (py < eff_cy0 || py >= eff_cy1) continue;
        int rem = r2 - y * y;
        if (rem < 0) continue;
        int max_x = 0;
        while ((max_x + 1) * (max_x + 1) <= rem) max_x++;

        int x0 = cx - max_x;
        int x1 = cx + max_x + 1;
        if (x0 < eff_cx0) x0 = eff_cx0;
        if (x1 > eff_cx1) x1 = eff_cx1;
        if (x0 >= x1) continue;

        unsigned int *row = &comp->backbuf[(unsigned int)py * pitch_px];
        for (int x = x0; x < x1; x++)
            row[x] = alpha_blend(row[x], color, alpha);
    }
}

static void __attribute__((unused)) bb_fill_circle_blend(az_compositor_t *comp, int cx, int cy, int r, unsigned int color)
{
    bb_fill_circle_blend_clipped(comp, cx, cy, r, color, 0, 0, (int)comp->fb_width, (int)comp->fb_height);
}

static void bb_fill_rounded_rect_clipped(az_compositor_t *comp, int rx, int ry, int rw, int rh, int radius,
                                         unsigned int color, int cx0, int cy0, int cx1, int cy1)
{
    if (rw <= 0 || rh <= 0) return;
    if (radius * 2 > rw) radius = rw / 2;
    if (radius * 2 > rh) radius = rh / 2;
    if (radius <= 0) {
        bb_fill_rect_clipped(comp, rx, ry, rw, rh, color, cx0, cy0, cx1, cy1);
        return;
    }
    bb_fill_rect_clipped(comp, rx + radius, ry, rw - 2 * radius, rh, color, cx0, cy0, cx1, cy1);
    bb_fill_rect_clipped(comp, rx, ry + radius, radius, rh - 2 * radius, color, cx0, cy0, cx1, cy1);
    bb_fill_rect_clipped(comp, rx + rw - radius, ry + radius, radius, rh - 2 * radius, color, cx0, cy0, cx1, cy1);
    bb_fill_circle_clipped(comp, rx + radius, ry + radius, radius, color, cx0, cy0, cx1, cy1);
    bb_fill_circle_clipped(comp, rx + rw - radius - 1, ry + radius, radius, color, cx0, cy0, cx1, cy1);
    bb_fill_circle_clipped(comp, rx + radius, ry + rh - radius - 1, radius, color, cx0, cy0, cx1, cy1);
    bb_fill_circle_clipped(comp, rx + rw - radius - 1, ry + rh - radius - 1, radius, color, cx0, cy0, cx1, cy1);
}

static void __attribute__((unused)) bb_fill_rounded_rect(az_compositor_t *comp, int rx, int ry, int rw, int rh, int radius, unsigned int color)
{
    bb_fill_rounded_rect_clipped(comp, rx, ry, rw, rh, radius, color, 0, 0, (int)comp->fb_width, (int)comp->fb_height);
}

static void draw_drop_shadow(az_compositor_t *comp, int rx, int ry, int rw, int rh,
                             int clip_x0, int clip_y0, int clip_x1, int clip_y1)
{
    if (!comp || !comp->backbuf) return;
    int s = 12;

    /* Quick reject if shadow bounding box doesn't touch clip */
    if (rx - s >= clip_x1 || rx + rw + s <= clip_x0 ||
        ry - s >= clip_y1 || ry + rh + s <= clip_y0) {
        return;
    }

    unsigned int pitch_px = comp->fb_pitch / 4;
    int fb_w = (int)comp->fb_width;
    int fb_h = (int)comp->fb_height;

    int eff_cx0 = clip_x0 < 0 ? 0 : clip_x0;
    int eff_cy0 = clip_y0 < 0 ? 0 : clip_y0;
    int eff_cx1 = clip_x1 > fb_w ? fb_w : clip_x1;
    int eff_cy1 = clip_y1 > fb_h ? fb_h : clip_y1;
    if (eff_cx0 >= eff_cx1 || eff_cy0 >= eff_cy1) return;

    unsigned int alpha_lut[16];
    for (int i = 0; i < s; i++) {
        int a = 120 - (i * 120 / s);
        alpha_lut[i] = (a > 0) ? (unsigned int)a : 0;
    }

    int x_start = rx < eff_cx0 ? eff_cx0 : rx;
    int x_end   = rx + rw > eff_cx1 ? eff_cx1 : rx + rw;
    int y_start = ry < eff_cy0 ? eff_cy0 : ry;
    int y_end   = ry + rh > eff_cy1 ? eff_cy1 : ry + rh;

    /* 1. Top strip */
    if (x_start < x_end) {
        int top_y_start = (ry - s) < eff_cy0 ? eff_cy0 : (ry - s);
        int top_y_end   = ry > eff_cy1 ? eff_cy1 : ry;
        for (int y = top_y_start; y < top_y_end; y++) {
            int dist = ry - y;
            if (dist < 0 || dist >= s) continue;
            unsigned int alpha = alpha_lut[dist];
            unsigned int *line = &comp->backbuf[y * pitch_px];
            alpha_shade_black_span(&line[x_start], alpha, x_end - x_start);
        }
    }

    /* 2. Bottom strip */
    if (x_start < x_end) {
        int bot_y_start = (ry + rh) < eff_cy0 ? eff_cy0 : (ry + rh);
        int bot_y_end   = (ry + rh + s) > eff_cy1 ? eff_cy1 : (ry + rh + s);
        for (int y = bot_y_start; y < bot_y_end; y++) {
            int dist = y - (ry + rh) + 1;
            if (dist < 0 || dist >= s) continue;
            unsigned int alpha = alpha_lut[dist];
            unsigned int *line = &comp->backbuf[y * pitch_px];
            alpha_shade_black_span(&line[x_start], alpha, x_end - x_start);
        }
    }

    /* 3. Left strip */
    if (y_start < y_end) {
        int left_x_start = (rx - s) < eff_cx0 ? eff_cx0 : (rx - s);
        int left_x_end   = rx > eff_cx1 ? eff_cx1 : rx;
        for (int x = left_x_start; x < left_x_end; x++) {
            int dist = rx - x;
            if (dist < 0 || dist >= s) continue;
            unsigned int alpha = alpha_lut[dist];
            for (int y = y_start; y < y_end; y++) {
                unsigned int *p = &comp->backbuf[y * pitch_px + x];
                *p = alpha_shade_black(*p, alpha);
            }
        }
    }

    /* 4. Right strip */
    if (y_start < y_end) {
        int right_x_start = (rx + rw) < eff_cx0 ? eff_cx0 : (rx + rw);
        int right_x_end   = (rx + rw + s) > eff_cx1 ? eff_cx1 : (rx + rw + s);
        for (int x = right_x_start; x < right_x_end; x++) {
            int dist = x - (rx + rw) + 1;
            if (dist < 0 || dist >= s) continue;
            unsigned int alpha = alpha_lut[dist];
            for (int y = y_start; y < y_end; y++) {
                unsigned int *p = &comp->backbuf[y * pitch_px + x];
                *p = alpha_shade_black(*p, alpha);
            }
        }
    }

    /* 5. Four Corners */
    int ctop_y_start = (ry - s) < eff_cy0 ? eff_cy0 : (ry - s);
    int ctop_y_end   = ry > eff_cy1 ? eff_cy1 : ry;
    for (int y = ctop_y_start; y < ctop_y_end; y++) {
        int dy = ry - y;
        /* Top-left */
        int tl_x_start = (rx - s) < eff_cx0 ? eff_cx0 : (rx - s);
        int tl_x_end   = rx > eff_cx1 ? eff_cx1 : rx;
        for (int x = tl_x_start; x < tl_x_end; x++) {
            int dx = rx - x;
            int dist = (dx > dy) ? dx : dy;
            if (dist < s) {
                unsigned int *p = &comp->backbuf[y * pitch_px + x];
                *p = alpha_shade_black(*p, alpha_lut[dist]);
            }
        }
        /* Top-right */
        int tr_x_start = (rx + rw) < eff_cx0 ? eff_cx0 : (rx + rw);
        int tr_x_end   = (rx + rw + s) > eff_cx1 ? eff_cx1 : (rx + rw + s);
        for (int x = tr_x_start; x < tr_x_end; x++) {
            int dx = x - (rx + rw) + 1;
            int dist = (dx > dy) ? dx : dy;
            if (dist < s) {
                unsigned int *p = &comp->backbuf[y * pitch_px + x];
                *p = alpha_shade_black(*p, alpha_lut[dist]);
            }
        }
    }

    int cbot_y_start = (ry + rh) < eff_cy0 ? eff_cy0 : (ry + rh);
    int cbot_y_end   = (ry + rh + s) > eff_cy1 ? eff_cy1 : (ry + rh + s);
    for (int y = cbot_y_start; y < cbot_y_end; y++) {
        int dy = y - (ry + rh) + 1;
        /* Bottom-left */
        int bl_x_start = (rx - s) < eff_cx0 ? eff_cx0 : (rx - s);
        int bl_x_end   = rx > eff_cx1 ? eff_cx1 : rx;
        for (int x = bl_x_start; x < bl_x_end; x++) {
            int dx = rx - x;
            int dist = (dx > dy) ? dx : dy;
            if (dist < s) {
                unsigned int *p = &comp->backbuf[y * pitch_px + x];
                *p = alpha_shade_black(*p, alpha_lut[dist]);
            }
        }
        /* Bottom-right */
        int br_x_start = (rx + rw) < eff_cx0 ? eff_cx0 : (rx + rw);
        int br_x_end   = (rx + rw + s) > eff_cx1 ? eff_cx1 : (rx + rw + s);
        for (int x = br_x_start; x < br_x_end; x++) {
            int dx = x - (rx + rw) + 1;
            int dist = (dx > dy) ? dx : dy;
            if (dist < s) {
                unsigned int *p = &comp->backbuf[y * pitch_px + x];
                *p = alpha_shade_black(*p, alpha_lut[dist]);
            }
        }
    }
}

/* ── Backdrop blur (AZ_WIN_FLAG_BLUR_BACKDROP) ────────────────────────────── */

/* Row/column scratch for blur_backdrop_region()'s two-pass separable box
 * blur — reused across both passes and every call, so blurring costs zero
 * allocation (this file otherwise never touches the heap). Sized for the
 * largest screen this compositor could plausibly run at; a row or column
 * longer than this just skips the blur for that call (see the bounds check
 * below) rather than overflow it — a window rendered sharp instead of
 * blurred is a degrade-gracefully condition here, not a correctness one. */
#define BLUR_SCRATCH_MAX 4096
#define BLUR_RADIUS      10

static unsigned int g_blur_src[BLUR_SCRATCH_MAX];
static unsigned int g_blur_dst[BLUR_SCRATCH_MAX];

/*
 * One box-blur pass over `count` pixels of `src` into `dst` — always two
 * distinct buffers, never the same one: the sliding window reads src[x -
 * radius] on the same iteration it has already overwritten dst[x - radius]
 * in an earlier step, so aliasing them would blur an already-blurred value
 * instead of the original.
 */
static void box_blur_line(const unsigned int *src, unsigned int *dst, int count, int radius)
{
    if (count <= 0) return;
    int window = 2 * radius + 1;
    long sum_r = 0, sum_g = 0, sum_b = 0;
    for (int i = -radius; i <= radius; i++) {
        int xi = i < 0 ? 0 : (i >= count ? count - 1 : i);
        unsigned int c = src[xi];
        sum_r += (c >> 16) & 0xFF;
        sum_g += (c >> 8) & 0xFF;
        sum_b += c & 0xFF;
    }
    for (int x = 0; x < count; x++) {
        dst[x] = 0xFF000000u |
                 (((unsigned int)(sum_r / window)) << 16) |
                 (((unsigned int)(sum_g / window)) << 8) |
                 ((unsigned int)(sum_b / window));
        int add_x = x + radius + 1; if (add_x >= count) add_x = count - 1;
        int sub_x = x - radius;     if (sub_x < 0)      sub_x = 0;
        unsigned int cadd = src[add_x];
        unsigned int csub = src[sub_x];
        sum_r += (long)((cadd >> 16) & 0xFF) - (long)((csub >> 16) & 0xFF);
        sum_g += (long)((cadd >> 8) & 0xFF)  - (long)((csub >> 8) & 0xFF);
        sum_b += (long)(cadd & 0xFF)         - (long)(csub & 0xFF);
    }
}

/*
 * blur_backdrop_region() — box-blur the backbuf rectangle [x0,y0)-(x1,y1) in
 * place: horizontal pass, then vertical pass over the result, each an O(n)
 * sliding-window average independent of BLUR_RADIUS.
 *
 * Called from render_window() for a window with blur_backdrop set, before
 * that window draws anything of its own. compose_screen() draws back-to-
 * front, so by the time render_window() reaches this window the backbuf
 * already holds everything behind it fully composited for *this* frame —
 * there is nothing to gather from other windows that is not already sitting
 * right here. Blurring it in place and letting this window's own (typically
 * translucent) content composite over the result next is the entire
 * "backdrop blur" effect; no separate off-screen composite pass is needed.
 *
 * Relies on compose_screen() having forced clip to the whole screen for
 * this frame (see its any_blur_backdrop check) whenever a blur_backdrop
 * window is visible: without that, a window below this one that sat
 * outside a narrower damage box would not have been redrawn this frame, and
 * blurring here would soften last frame's *already blurred* pixels a
 * little further instead of a fresh composite — visibly over-blurring a
 * little more on every such frame.
 */
static void blur_backdrop_region(az_compositor_t *comp, int x0, int y0, int x1, int y1)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int)comp->fb_width)  x1 = (int)comp->fb_width;
    if (y1 > (int)comp->fb_height) y1 = (int)comp->fb_height;
    if (x1 <= x0 || y1 <= y0) return;

    int w = x1 - x0;
    int h = y1 - y0;
    if (w > BLUR_SCRATCH_MAX || h > BLUR_SCRATCH_MAX) return;

    unsigned int pitch_px = comp->fb_pitch / 4;

    /* Horizontal pass, one row at a time. */
    for (int y = y0; y < y1; y++) {
        unsigned int *row = &comp->backbuf[(unsigned int)y * pitch_px + (unsigned int)x0];
        memcpy(g_blur_src, row, (size_t)w * sizeof(unsigned int));
        box_blur_line(g_blur_src, g_blur_dst, w, BLUR_RADIUS);
        memcpy(row, g_blur_dst, (size_t)w * sizeof(unsigned int));
    }

    /* Vertical pass, one column at a time, over the horizontally-blurred
     * result above. */
    for (int x = x0; x < x1; x++) {
        for (int y = 0; y < h; y++) {
            g_blur_src[y] = comp->backbuf[(unsigned int)(y0 + y) * pitch_px + (unsigned int)x];
        }
        box_blur_line(g_blur_src, g_blur_dst, h, BLUR_RADIUS);
        for (int y = 0; y < h; y++) {
            comp->backbuf[(unsigned int)(y0 + y) * pitch_px + (unsigned int)x] = g_blur_dst[y];
        }
    }
}

/*
 * @clip_x0/y0/x1/y1 bound the region compositor_present() is actually going
 * to copy out this frame (compose_screen derives it from the damage box, or
 * the full screen when nothing narrowed it). Only the client-area blit below
 * is clipped to it — that SIMD alpha blend is the costliest part of drawing
 * a window and the part damage most often shrinks to almost nothing (a text
 * caret in an otherwise static, maximized terminal), so narrowing just that
 * loop turns the expensive part from O(window area) into O(damage area)
 * without changing a single pixel that would reach the screen: anything
 * outside the clip is, under the same has_damage contract compositor_present()
 * already relies on, unchanged from the backbuf's last correct composite of
 * it. The frame chrome (border, titlebar, buttons, shadow) stays unclipped
 * since it is cheap and callers already skip this whole function when a
 * window's frame+shadow bounds miss the clip entirely.
 */
static void render_titlebar_icon(az_compositor_t *comp, const char *title, int ix, int iy,
                                 int clip_x0, int clip_y0, int clip_x1, int clip_y1)
{
    int w = 14, h = 14;
    if (ix + w <= clip_x0 || ix >= clip_x1 || iy + h <= clip_y0 || iy >= clip_y1) return;
    if (ix < 0 || ix + w >= (int)comp->fb_width || iy < 0 || iy + h >= (int)comp->fb_height) return;

    bool is_term = (strstr(title, "Terminal") != 0 || strstr(title, "sh") != 0);
    bool is_files = (strstr(title, "File") != 0);
    bool is_settings = (strstr(title, "Settings") != 0 || strstr(title, "Config") != 0);
    bool is_edit = (strstr(title, "Edit") != 0 || strstr(title, "Text") != 0);
    bool is_sysmon = (strstr(title, "Monitor") != 0 || strstr(title, "Sysmon") != 0);
    bool is_calc = (strstr(title, "Calc") != 0);

    unsigned int bg_col = 0xFF181825;
    unsigned int badge_col = 0xFF89B4FA;
    if (is_term) badge_col = 0xFFA6E3A1;
    else if (is_files) badge_col = 0xFFFAB387;
    else if (is_settings) badge_col = 0xFFB4BEFE;
    else if (is_edit) badge_col = 0xFF94E2D5;
    else if (is_sysmon) badge_col = 0xFFF38BA8;
    else if (is_calc) badge_col = 0xFFF9E2AF;

    bb_fill_rounded_rect_clipped(comp, ix, iy, w, h, 3, bg_col, clip_x0, clip_y0, clip_x1, clip_y1);

    if (is_term) {
        bb_put_pixel_clipped(comp, ix + 3, iy + 3, badge_col, clip_x0, clip_y0, clip_x1, clip_y1);
        bb_put_pixel_clipped(comp, ix + 4, iy + 4, badge_col, clip_x0, clip_y0, clip_x1, clip_y1);
        bb_put_pixel_clipped(comp, ix + 3, iy + 5, badge_col, clip_x0, clip_y0, clip_x1, clip_y1);
        bb_put_pixel_clipped(comp, ix + 6, iy + 8, 0xFFCDD6F4, clip_x0, clip_y0, clip_x1, clip_y1);
        bb_put_pixel_clipped(comp, ix + 7, iy + 8, 0xFFCDD6F4, clip_x0, clip_y0, clip_x1, clip_y1);
        bb_put_pixel_clipped(comp, ix + 8, iy + 8, 0xFFCDD6F4, clip_x0, clip_y0, clip_x1, clip_y1);
    } else if (is_files) {
        for (int x = ix + 2; x <= ix + 6; x++)
            bb_put_pixel_clipped(comp, x, iy + 2, badge_col, clip_x0, clip_y0, clip_x1, clip_y1);
        for (int y = iy + 4; y <= iy + 10; y++) {
            for (int x = ix + 2; x <= ix + 11; x++)
                bb_put_pixel_clipped(comp, x, y, badge_col, clip_x0, clip_y0, clip_x1, clip_y1);
        }
    } else if (is_settings) {
        bb_fill_circle_clipped(comp, ix + 7, iy + 7, 4, badge_col, clip_x0, clip_y0, clip_x1, clip_y1);
        bb_fill_circle_clipped(comp, ix + 7, iy + 7, 2, bg_col, clip_x0, clip_y0, clip_x1, clip_y1);
        bb_put_pixel_clipped(comp, ix + 7, iy + 2, badge_col, clip_x0, clip_y0, clip_x1, clip_y1);
        bb_put_pixel_clipped(comp, ix + 7, iy + 12, badge_col, clip_x0, clip_y0, clip_x1, clip_y1);
        bb_put_pixel_clipped(comp, ix + 2, iy + 7, badge_col, clip_x0, clip_y0, clip_x1, clip_y1);
        bb_put_pixel_clipped(comp, ix + 12, iy + 7, badge_col, clip_x0, clip_y0, clip_x1, clip_y1);
    } else if (is_edit) {
        for (int y = iy + 2; y <= iy + 11; y++) {
            for (int x = ix + 3; x <= ix + 10; x++)
                bb_put_pixel_clipped(comp, x, y, 0xFFCDD6F4, clip_x0, clip_y0, clip_x1, clip_y1);
        }
        for (int x = ix + 5; x <= ix + 8; x++)
            bb_put_pixel_clipped(comp, x, iy + 5, badge_col, clip_x0, clip_y0, clip_x1, clip_y1);
        for (int x = ix + 5; x <= ix + 8; x++)
            bb_put_pixel_clipped(comp, x, iy + 7, badge_col, clip_x0, clip_y0, clip_x1, clip_y1);
        for (int x = ix + 5; x <= ix + 7; x++)
            bb_put_pixel_clipped(comp, x, iy + 9, badge_col, clip_x0, clip_y0, clip_x1, clip_y1);
    } else {
        bb_fill_circle_clipped(comp, ix + 7, iy + 7, 3, badge_col, clip_x0, clip_y0, clip_x1, clip_y1);
    }
}

static void render_window(az_compositor_t *comp, az_window_t *win,
                          int clip_x0, int clip_y0, int clip_x1, int clip_y1)
{
    if (!win->visible || win->wid == 0) return;

    bool has_frame = win_has_frame(win);

    int wx = win->x;
    int wy = win->y;
    int ww = (int)win->width;
    int wh = (int)win->height;

    if (win->blur_backdrop) {
        int bx0 = wx, by0 = wy, bx1 = wx + ww, by1 = wy + wh;
        if (has_frame) {
            bx0 -= AZWM_BORDER_W;
            by0 -= (AZWM_TITLEBAR_H + AZWM_BORDER_W);
            bx1 += AZWM_BORDER_W;
            by1 += AZWM_BORDER_W;
        }
        blur_backdrop_region(comp, bx0, by0, bx1, by1);
    }

    if (has_frame) {
        int fx = wx - AZWM_BORDER_W;
        int fy = wy - AZWM_TITLEBAR_H - AZWM_BORDER_W;
        int fw = ww + 2 * AZWM_BORDER_W;
        int fh = wh + AZWM_TITLEBAR_H + 2 * AZWM_BORDER_W;

        if (win->focused) {
            draw_drop_shadow(comp, fx, fy, fw, fh, clip_x0, clip_y0, clip_x1, clip_y1);
        }

        /* Border / frame: focused = blue tint, unfocused = surface.
         * We draw ONLY the outer perimeter strips; we NEVER fill the client area [wx, wy, ww, wh]! */
        unsigned int border_color = win->focused ? 0xFF89B4FA : 0xFF45475A;

        /* 1. Top border */
        bb_fill_rect_clipped(comp, fx, fy, fw, AZWM_BORDER_W,
                             border_color, clip_x0, clip_y0, clip_x1, clip_y1);
        /* 2. Left border */
        bb_fill_rect_clipped(comp, fx, wy - AZWM_TITLEBAR_H, AZWM_BORDER_W,
                             wh + AZWM_TITLEBAR_H + AZWM_BORDER_W, border_color,
                             clip_x0, clip_y0, clip_x1, clip_y1);
        /* 3. Right border */
        bb_fill_rect_clipped(comp, wx + ww, wy - AZWM_TITLEBAR_H, AZWM_BORDER_W,
                             wh + AZWM_TITLEBAR_H + AZWM_BORDER_W, border_color,
                             clip_x0, clip_y0, clip_x1, clip_y1);
        /* 4. Bottom border */
        bb_fill_rect_clipped(comp, fx, wy + wh, fw, AZWM_BORDER_W,
                             border_color, clip_x0, clip_y0, clip_x1, clip_y1);

        /* Gradient titlebar header (strictly between top border and client area wy) */
        int tb_top_y = wy - AZWM_TITLEBAR_H;
        int tb_bot_y = wy;
        int tb_range = tb_bot_y - tb_top_y;

        if (wx < clip_x1 && wx + ww > clip_x0 && tb_top_y < clip_y1 && tb_bot_y > clip_y0) {
            unsigned int pitch_px2 = comp->fb_pitch / 4;
            int ty_start = tb_top_y < clip_y0 ? clip_y0 : tb_top_y;
            int ty_end   = tb_bot_y > clip_y1 ? clip_y1 : tb_bot_y;
            int tx_start = wx < clip_x0 ? clip_x0 : wx;
            int tx_end   = (wx + ww) > clip_x1 ? clip_x1 : (wx + ww);

            for (int ty2 = ty_start; ty2 < ty_end; ty2++) {
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
                if (tx_start < tx_end) {
                    bb_fill_span(&comp->backbuf[ty2 * pitch_px2 + tx_start], col, tx_end - tx_start);
                }
            }

            /* Specular glass highlight line on top edge of titlebar */
            if (tb_top_y >= clip_y0 && tb_top_y < clip_y1 && tb_top_y >= 0 && tb_top_y < (int)comp->fb_height) {
                unsigned int highlight = win->focused ? 0x60FFFFFF : 0x25FFFFFF;
                unsigned int *hl_line = &comp->backbuf[tb_top_y * pitch_px2];
                int hx0 = (wx + 2) < clip_x0 ? clip_x0 : (wx + 2);
                int hx1 = (wx + ww - 2) > clip_x1 ? clip_x1 : (wx + ww - 2);
                for (int tx2 = hx0; tx2 < hx1; tx2++) {
                    hl_line[tx2] = alpha_blend(hl_line[tx2], 0xFFFFFFFF, (highlight >> 24) & 0xFF);
                }
            }

            /* Titlebar icon */
            render_titlebar_icon(comp, win->title, wx + 6, wy - AZWM_TITLEBAR_H + 5,
                                 clip_x0, clip_y0, clip_x1, clip_y1);

            /* Window title text */
            int tx = wx + 24;
            int ty = wy - AZWM_TITLEBAR_H + 4;
            for (int i = 0; win->title[i] && tx + 8 < wx + ww - 65; i++) {
                if (tx + 8 > clip_x0 && tx < clip_x1 && ty + 12 > clip_y0 && ty < clip_y1) {
                    desktop_draw_char_at(comp->backbuf, comp->fb_width, comp->fb_height,
                                         comp->fb_pitch / 4, tx + 1, ty + 1, win->title[i],
                                         0xFF11111B);
                    desktop_draw_char_at(comp->backbuf, comp->fb_width, comp->fb_height,
                                         comp->fb_pitch / 4, tx, ty, win->title[i],
                                         win->focused ? 0xFFCDD6F4 : 0xFF6C7086);
                }
                tx += 8;
            }

            if (win->pinned) {
                bb_fill_circle_clipped(comp, wx + ww - 75, wy - AZWM_TITLEBAR_H + 12, 3, 0xFFF9E2AF,
                                       clip_x0, clip_y0, clip_x1, clip_y1);
            }

            /* Decoration buttons */
            bool hov_this_win = (comp->hover_btn_wid == win->wid);
            bool hov_close = hov_this_win && comp->hover_btn == AZWM_BTN_CLOSE;
            bool hov_min   = hov_this_win && comp->hover_btn == AZWM_BTN_MIN;
            bool hov_max   = hov_this_win && comp->hover_btn == AZWM_BTN_MAX;

            /* Close button — red circle with × glyph */
            int close_x = wx + ww - 18;
            int close_y = wy - AZWM_TITLEBAR_H + 12;
            if (hov_close) bb_fill_circle_blend_clipped(comp, close_x, close_y, 9, 0x60F38BA8, clip_x0, clip_y0, clip_x1, clip_y1);
            bb_fill_circle_clipped(comp, close_x, close_y, 7, 0xFFF38BA8, clip_x0, clip_y0, clip_x1, clip_y1);
            bb_put_pixel_clipped(comp, close_x - 2, close_y - 2, 0xFF1E1E2E, clip_x0, clip_y0, clip_x1, clip_y1);
            bb_put_pixel_clipped(comp, close_x - 1, close_y - 1, 0xFF1E1E2E, clip_x0, clip_y0, clip_x1, clip_y1);
            bb_put_pixel_clipped(comp, close_x,     close_y,     0xFF1E1E2E, clip_x0, clip_y0, clip_x1, clip_y1);
            bb_put_pixel_clipped(comp, close_x + 1, close_y + 1, 0xFF1E1E2E, clip_x0, clip_y0, clip_x1, clip_y1);
            bb_put_pixel_clipped(comp, close_x + 2, close_y + 2, 0xFF1E1E2E, clip_x0, clip_y0, clip_x1, clip_y1);
            bb_put_pixel_clipped(comp, close_x + 2, close_y - 2, 0xFF1E1E2E, clip_x0, clip_y0, clip_x1, clip_y1);
            bb_put_pixel_clipped(comp, close_x + 1, close_y - 1, 0xFF1E1E2E, clip_x0, clip_y0, clip_x1, clip_y1);
            bb_put_pixel_clipped(comp, close_x - 1, close_y + 1, 0xFF1E1E2E, clip_x0, clip_y0, clip_x1, clip_y1);
            bb_put_pixel_clipped(comp, close_x - 2, close_y + 2, 0xFF1E1E2E, clip_x0, clip_y0, clip_x1, clip_y1);

            /* Minimize button — yellow circle with − glyph */
            int min_x = wx + ww - 38;
            int min_y = wy - AZWM_TITLEBAR_H + 12;
            if (hov_min) bb_fill_circle_blend_clipped(comp, min_x, min_y, 9, 0x60F9E2AF, clip_x0, clip_y0, clip_x1, clip_y1);
            bb_fill_circle_clipped(comp, min_x, min_y, 7, 0xFFF9E2AF, clip_x0, clip_y0, clip_x1, clip_y1);
            bb_fill_rect_clipped(comp, min_x - 3, min_y, 7, 2, 0xFF1E1E2E, clip_x0, clip_y0, clip_x1, clip_y1);

            /* Maximize button — green circle with □ glyph */
            int max_x = wx + ww - 58;
            int max_y = wy - AZWM_TITLEBAR_H + 12;
            if (hov_max) bb_fill_circle_blend_clipped(comp, max_x, max_y, 9, 0x60A6E3A1, clip_x0, clip_y0, clip_x1, clip_y1);
            bb_fill_circle_clipped(comp, max_x, max_y, 7, 0xFFA6E3A1, clip_x0, clip_y0, clip_x1, clip_y1);
            bb_fill_rect_clipped(comp, max_x - 3, max_y - 3, 7, 1, 0xFF1E1E2E, clip_x0, clip_y0, clip_x1, clip_y1);
            bb_fill_rect_clipped(comp, max_x - 3, max_y + 3, 7, 1, 0xFF1E1E2E, clip_x0, clip_y0, clip_x1, clip_y1);
            bb_fill_rect_clipped(comp, max_x - 3, max_y - 2, 1, 5, 0xFF1E1E2E, clip_x0, clip_y0, clip_x1, clip_y1);
        }
    }

    /* Client area fallback if client pixel buffer is not yet attached */
    if (!win->pixels || (unsigned long)win->pixels < 0x40000000UL) {
        bb_fill_rect_clipped(comp, wx, wy, ww, wh, 0xFF1E1E2E, clip_x0, clip_y0, clip_x1, clip_y1);
    }

    if (win->pixels && (unsigned long)win->pixels >= 0x40000000UL) {
        unsigned int pitch_px = comp->fb_pitch / 4;
        int max_rows = wh;
        if (win->buffer_h > 0 && max_rows > (int)win->buffer_h)
            max_rows = (int)win->buffer_h;

        int row_lo = 0, row_hi = max_rows;
        {
            int lo = clip_y0 - wy;
            int hi = clip_y1 - wy;
            if (lo > row_lo) row_lo = lo;
            if (hi < row_hi) row_hi = hi;
        }

        for (int row = row_lo; row < row_hi; row++) {
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
            if (dst_x < clip_x0) {
                int adj = clip_x0 - dst_x;
                src_x  += adj;
                copy_w -= adj;
                dst_x   = clip_x0;
            }
            if (dst_x + copy_w > clip_x1) {
                copy_w = clip_x1 - dst_x;
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

                    /* High-speed SIMD-vectorized alpha blending with window opacity */
                    composite_blend_span_opacity(dst_ptr, src_ptr, copy_w, win->opacity);
                }
            }
        }
    }
    /* Render resize grip in bottom-right corner of framed windows */
    if (has_frame && !win->maximized) {
        int rx = wx + ww - 10;
        int ry = wy + wh - 10;
        if (rx + 10 > clip_x0 && rx < clip_x1 && ry + 10 > clip_y0 && ry < clip_y1) {
            unsigned int grip_col = win->focused ? 0xFF89B4FA : 0xFF585B70;
            if (comp->hover_btn_wid == win->wid && comp->hover_btn == AZWM_BTN_RESIZE)
                bb_fill_circle_blend_clipped(comp, rx + 4, ry + 4, 9, 0x5089B4FA, clip_x0, clip_y0, clip_x1, clip_y1);
            bb_put_pixel_clipped(comp, rx + 6, ry + 6, grip_col, clip_x0, clip_y0, clip_x1, clip_y1);
            bb_put_pixel_clipped(comp, rx + 4, ry + 6, grip_col, clip_x0, clip_y0, clip_x1, clip_y1);
            bb_put_pixel_clipped(comp, rx + 6, ry + 4, grip_col, clip_x0, clip_y0, clip_x1, clip_y1);
            bb_put_pixel_clipped(comp, rx + 2, ry + 6, grip_col, clip_x0, clip_y0, clip_x1, clip_y1);
            bb_put_pixel_clipped(comp, rx + 4, ry + 4, grip_col, clip_x0, clip_y0, clip_x1, clip_y1);
            bb_put_pixel_clipped(comp, rx + 6, ry + 2, grip_col, clip_x0, clip_y0, clip_x1, clip_y1);
        }
    }
}

static void draw_snap_preview(az_compositor_t *comp, int clip_x0, int clip_y0, int clip_x1, int clip_y1)
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
    if (sx >= clip_x1 || sx + sw <= clip_x0 || sy >= clip_y1 || sy + sh <= clip_y0) return;

    unsigned int pitch_px = comp->fb_pitch / 4;
    int y0_cl = sy < clip_y0 ? clip_y0 : sy;
    int y1_cl = (sy + sh) > clip_y1 ? clip_y1 : (sy + sh);
    int x0_cl = sx < clip_x0 ? clip_x0 : sx;
    int x1_cl = (sx + sw) > clip_x1 ? clip_x1 : (sx + sw);

    /* Translucent frosted glass tint (0x4589B4FA) */
    for (int y = y0_cl; y < y1_cl; y++) {
        if (y < 0 || y >= fb_h) continue;
        unsigned int *line = &comp->backbuf[y * pitch_px];
        for (int x = x0_cl; x < x1_cl; x++) {
            if (x < 0 || x >= fb_w) continue;
            line[x] = alpha_blend(line[x], 0xFF89B4FA, 68);
        }
    }

    /* Glowing outline borders */
    bb_fill_rect_clipped(comp, sx, sy, sw, 2, 0xFF89B4FA, clip_x0, clip_y0, clip_x1, clip_y1);
    bb_fill_rect_clipped(comp, sx, sy + sh - 2, sw, 2, 0xFF89B4FA, clip_x0, clip_y0, clip_x1, clip_y1);
    bb_fill_rect_clipped(comp, sx, sy, 2, sh, 0xFF89B4FA, clip_x0, clip_y0, clip_x1, clip_y1);
    bb_fill_rect_clipped(comp, sx + sw - 2, sy, 2, sh, 0xFF89B4FA, clip_x0, clip_y0, clip_x1, clip_y1);
}

static void draw_alt_tab_hud(az_compositor_t *comp, int clip_x0, int clip_y0, int clip_x1, int clip_y1)
{
    if (!comp->alt_tab_active || comp->alt_tab_count <= 0) return;

    int hud_w = 460;
    int hud_h = 56 + comp->alt_tab_count * 34;
    int hx = ((int)comp->fb_width - hud_w) / 2;
    int hy = ((int)comp->fb_height - hud_h) / 2;
    if (hx < 0) hx = 0;
    if (hy < 0) hy = 0;

    if (hx - 16 >= clip_x1 || hx + hud_w + 16 <= clip_x0 ||
        hy - 16 >= clip_y1 || hy + hud_h + 16 <= clip_y0) return;

    /* Drop shadow for HUD */
    draw_drop_shadow(comp, hx, hy, hud_w, hud_h, clip_x0, clip_y0, clip_x1, clip_y1);

    /* Background panel with rounded rect */
    bb_fill_rounded_rect_clipped(comp, hx, hy, hud_w, hud_h, 12, 0xFF181825, clip_x0, clip_y0, clip_x1, clip_y1);
    /* Border outline */
    bb_fill_rect_clipped(comp, hx, hy, hud_w, 2, 0xFFB4BEFE, clip_x0, clip_y0, clip_x1, clip_y1);

    /* Header title */
    const char *hdr = "Active Applications (Alt + Tab)";
    for (int i = 0; hdr[i]; i++) {
        int cx = hx + 18 + i * 8;
        int cy = hy + 14;
        if (cx + 8 > clip_x0 && cx < clip_x1 && cy + 12 > clip_y0 && cy < clip_y1) {
            desktop_draw_char_at(comp->backbuf, comp->fb_width, comp->fb_height,
                                 comp->fb_pitch / 4, cx, cy, hdr[i], 0xFFCBA6F7);
        }
    }

    /* Separator line */
    bb_fill_rect_clipped(comp, hx + 16, hy + 38, hud_w - 32, 1, 0xFF313244, clip_x0, clip_y0, clip_x1, clip_y1);

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
        if (iy + 34 <= clip_y0 || iy >= clip_y1) continue;
        bool is_sel = (i == comp->alt_tab_idx);

        if (is_sel) {
            bb_fill_rounded_rect_clipped(comp, hx + 12, iy, hud_w - 24, 28, 6, 0xFF313244, clip_x0, clip_y0, clip_x1, clip_y1);
            bb_fill_rounded_rect_clipped(comp, hx + 14, iy + 4, 4, 20, 2, 0xFF89B4FA, clip_x0, clip_y0, clip_x1, clip_y1);
        }

        /* Bullet / app icon dot */
        bb_fill_circle_clipped(comp, hx + 28, iy + 14, 4, is_sel ? 0xFFA6E3A1 : 0xFF585B70, clip_x0, clip_y0, clip_x1, clip_y1);

        /* Window title text */
        const char *t = win->title[0] ? win->title : "Application Window";
        int tx = hx + 40;
        for (int c = 0; t[c] && tx < hx + hud_w - 30; c++) {
            if (tx + 8 > clip_x0 && tx < clip_x1 && iy + 10 + 12 > clip_y0 && iy + 10 < clip_y1) {
                desktop_draw_char_at(comp->backbuf, comp->fb_width, comp->fb_height,
                                     comp->fb_pitch / 4, tx, iy + 10, t[c],
                                     is_sel ? 0xFFCDD6F4 : 0xFF9399B2);
            }
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

static void draw_context_menu(az_compositor_t *comp, int clip_x0, int clip_y0, int clip_x1, int clip_y1)
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

    if (mx - 16 >= clip_x1 || mx + mw + 16 <= clip_x0 ||
        my - 16 >= clip_y1 || my + mh + 16 <= clip_y0) return;

    /* Shadow */
    draw_drop_shadow(comp, mx, my, mw, mh, clip_x0, clip_y0, clip_x1, clip_y1);

    /* Main Menu Frame with top accent */
    bb_fill_rounded_rect_clipped(comp, mx, my, mw, mh, 8, 0xFF181825, clip_x0, clip_y0, clip_x1, clip_y1);
    bb_fill_rect_clipped(comp, mx, my, mw, 2, 0xFF89B4FA, clip_x0, clip_y0, clip_x1, clip_y1);

    /* Render Menu Items */
    for (size_t i = 0; i < CTX_MENU_COUNT; i++) {
        int iy = my + 6 + (int)i * 26;
        if (iy + 26 <= clip_y0 || iy >= clip_y1) continue;
        bool is_hov = ((int)i == comp->ctx_menu_hover);

        if (is_hov) {
            bb_fill_rounded_rect_clipped(comp, mx + 6, iy, mw - 12, 24, 4, 0xFF313244, clip_x0, clip_y0, clip_x1, clip_y1);
            bb_fill_rounded_rect_clipped(comp, mx + 8, iy + 4, 3, 16, 2, 0xFF89B4FA, clip_x0, clip_y0, clip_x1, clip_y1);
        }

        /* Dot indicator */
        unsigned int dot_color = is_hov ? 0xFF89B4FA : 0xFF6C7086;
        bb_fill_circle_clipped(comp, mx + 18, iy + 12, 3, dot_color, clip_x0, clip_y0, clip_x1, clip_y1);

        /* Label */
        desktop_draw_text_at(comp->backbuf, comp->fb_width, comp->fb_height,
                             comp->fb_pitch / 4, mx + 28, iy + 6,
                             g_ctx_menu_items[i],
                             is_hov ? 0xFFFFFFFF : 0xFFCDD6F4);
    }
}

/* On-screen frame-rate counter, F12-toggled — reads current_fps as sampled
 * by compositor_present_internal(). */
static void draw_fps_hud(az_compositor_t *comp, int clip_x0, int clip_y0, int clip_x1, int clip_y1)
{
    if (!comp->fps_hud_visible) return;

    char label[16];
    int len = snprintf(label, sizeof(label), "%u FPS", comp->current_fps);
    if (len < 0) return;
    if ((size_t)len >= sizeof(label)) len = (int)sizeof(label) - 1;

    int w = 16 + len * 8;
    int h = 24;
    int x = (int)comp->fb_width - w - 10;
    int y = 10;
    if (x < 0) x = 0;

    if (x >= clip_x1 || x + w <= clip_x0 || y >= clip_y1 || y + h <= clip_y0) return;

    bb_fill_rounded_rect_clipped(comp, x, y, w, h, 6, 0xFF181825, clip_x0, clip_y0, clip_x1, clip_y1);
    unsigned int color = comp->current_fps >= 50 ? 0xFFA6E3A1
                        : comp->current_fps >= 30 ? 0xFFF9E2AF
                        : 0xFFF38BA8;
    desktop_draw_text_at(comp->backbuf, comp->fb_width, comp->fb_height,
                         comp->fb_pitch / 4, x + 8, y + 8, label, color);
}

/* ── Presentation ────────────────────────────────────────────────────────── */

static void rect_union(azwm_rect_t *dst, int x0, int y0, int x1, int y1)
{
    if (x0 >= x1 || y0 >= y1) return;
    if (!dst->valid) {
        dst->x0 = x0; dst->y0 = y0; dst->x1 = x1; dst->y1 = y1;
        dst->valid = 1;
        return;
    }
    if (x0 < dst->x0) dst->x0 = x0;
    if (y0 < dst->y0) dst->y0 = y0;
    if (x1 > dst->x1) dst->x1 = x1;
    if (y1 > dst->y1) dst->y1 = y1;
}

static void rect_union_rect(azwm_rect_t *dst, const azwm_rect_t *src)
{
    if (src->valid) rect_union(dst, src->x0, src->y0, src->x1, src->y1);
}

/* The pointer sprite plus the hotspot margins used across cursor types. */
static azwm_rect_t cursor_bounds(const az_compositor_t *comp, int cx, int cy)
{
    azwm_rect_t r;
    r.x0 = cx - 8;
    r.y0 = cy - 8;
    r.x1 = cx + DESKTOP_CURSOR_W + 8;
    r.y1 = cy + DESKTOP_CURSOR_H + 8;
    if (r.x0 < 0) r.x0 = 0;
    if (r.y0 < 0) r.y0 = 0;
    if (r.x1 > (int)comp->fb_width)  r.x1 = (int)comp->fb_width;
    if (r.y1 > (int)comp->fb_height) r.y1 = (int)comp->fb_height;
    r.valid = (r.x0 < r.x1 && r.y0 < r.y1);
    return r;
}

__attribute__((target("avx2")))
static void copy_rect_avx2(az_compositor_t *comp, unsigned int *dst,
                           const azwm_rect_t *r)
{
    unsigned int pitch_px = comp->fb_pitch / 4;
    int w = r->x1 - r->x0;

    for (int y = r->y0; y < r->y1; y++) {
        unsigned int *dst_row = &dst[(unsigned int)y * pitch_px + (unsigned int)r->x0];
        const unsigned int *src_row = &comp->backbuf[(unsigned int)y * pitch_px + (unsigned int)r->x0];

        int px = 0;
        /* Align to 32-byte boundary for AVX2 streaming stores */
        while (((uintptr_t)&dst_row[px] & 31) && px < w) {
            dst_row[px] = src_row[px];
            px++;
        }
        /* 256-bit streaming non-temporal stores direct to write-combining VRAM */
        while (px + 8 <= w) {
            __m256i v = _mm256_loadu_si256((const __m256i *)&src_row[px]);
            _mm256_stream_si256((__m256i *)&dst_row[px], v);
            px += 8;
        }
        while (px + 4 <= w) {
            __m128i v = _mm_loadu_si128((const __m128i *)&src_row[px]);
            _mm_stream_si128((__m128i *)&dst_row[px], v);
            px += 4;
        }
        while (px < w) {
            dst_row[px] = src_row[px];
            px++;
        }
    }
    _mm_sfence();
}

static void copy_rect(az_compositor_t *comp, unsigned int *dst,
                      const azwm_rect_t *r)
{
    if (!r->valid) return;
    int w = r->x1 - r->x0;
    if (w <= 0) return;

    if (compositor_cpu_has_avx2()) {
        copy_rect_avx2(comp, dst, r);
        return;
    }

    unsigned int pitch_px = comp->fb_pitch / 4;
    for (int y = r->y0; y < r->y1; y++) {
        unsigned int *dst_row = &dst[(unsigned int)y * pitch_px + (unsigned int)r->x0];
        const unsigned int *src_row = &comp->backbuf[(unsigned int)y * pitch_px + (unsigned int)r->x0];

        int px = 0;
        /* Align to 16-byte boundary for streaming stores */
        while (((uintptr_t)&dst_row[px] & 15) && px < w) {
            dst_row[px] = src_row[px];
            px++;
        }
        /* 128-bit streaming non-temporal stores direct to write-combining VRAM */
        while (px + 4 <= w) {
            __m128i v = _mm_loadu_si128((const __m128i *)&src_row[px]);
            _mm_stream_si128((__m128i *)&dst_row[px], v);
            px += 4;
        }
        while (px < w) {
            dst_row[px] = src_row[px];
            px++;
        }
    }
    _mm_sfence();
}

void compositor_enable_page_flip(az_compositor_t *comp, int fb_fd,
                                 unsigned int *vram, unsigned int yres)
{
    unsigned int pitch_px = comp->fb_pitch / 4;

    /* Compositing has to happen somewhere other than video memory, or the
     * second buffer buys nothing. */
    if (!vram || comp->backbuf == vram || fb_fd < 0) return;

    comp->fb_fd           = fb_fd;
    comp->fb_yres         = yres;
    comp->vram_buf[0]     = vram;
    comp->vram_buf[1]     = vram + (size_t)yres * pitch_px;
    comp->active_vram_buf = 0;
    comp->frontbuf        = comp->vram_buf[0];
    comp->hw_page_flip    = 1;

    /* Neither buffer holds anything yet. */
    comp->pending[0].valid = comp->pending[1].valid = 0;
    rect_union(&comp->pending[0], 0, 0, (int)comp->fb_width, (int)comp->fb_height);
    rect_union(&comp->pending[1], 0, 0, (int)comp->fb_width, (int)comp->fb_height);
    comp->cursor_rect[0].valid = comp->cursor_rect[1].valid = 0;
}

void compositor_enable_hw_cursor(az_compositor_t *comp, int fb_fd)
{
    if (!comp || fb_fd < 0) return;

    /* 64x64x4 = 16 KiB; static because this runs once at init and a frame that
     * big does not belong on the stack. */
    static unsigned int img[FB_AZ_HWCURSOR_MAX * FB_AZ_HWCURSOR_MAX];
    memset(img, 0, sizeof(img));
    desktop_cursor_blit_bgra(img, FB_AZ_HWCURSOR_MAX, FB_AZ_HWCURSOR_MAX);

    struct fb_az_hwcursor c = {
        .width  = DESKTOP_CURSOR_W,
        .height = DESKTOP_CURSOR_H,
        .hot_x  = DESKTOP_CURSOR_HX,
        .hot_y  = DESKTOP_CURSOR_HY,
        .image  = (uint64_t)(uintptr_t)img,
    };

    if (ioctl(fb_fd, FBIOAZ_HWCURSOR_SET, &c) != 0)
        return;                        /* no overlay — stay on the software path */

    comp->hw_cursor    = 1;
    comp->hw_cursor_fd = fb_fd;

    struct fb_az_hwcursor_pos p = { comp->cursor_x, comp->cursor_y };
    ioctl(fb_fd, FBIOAZ_HWCURSOR_MOVE, &p);
}

/* Move the scanout to @buf.  The kernel waits for the frame boundary. */
static int display_pan(az_compositor_t *comp, int buf)
{
    struct fb_var_screeninfo var;

    if (comp->fb_fd >= 0 && ioctl(comp->fb_fd, FBIOGET_VSCREENINFO, &var) == 0) {
        var.xoffset  = 0;
        var.yoffset  = (unsigned int)buf * comp->fb_yres;
        var.activate = FB_ACTIVATE_VBL;
        if (ioctl(comp->fb_fd, FBIOPAN_DISPLAY, &var) == 0) return 0;
    }
    /* Older kernels only have the direct flip call. */
    return az_fb_flip((unsigned int)buf);
}

/*
 * Put the composed frame on screen.
 *
 * Single-buffered, there is no way to avoid writing to memory the display is
 * reading, so the copy is kept as small as the damage allows.  Double
 * buffered, the copy goes into the buffer that is *not* being scanned out and
 * the swap is a pan at the frame boundary: the screen only ever shows a whole
 * frame, which is what removes the flicker of a compositor painting live
 * video memory.
 */
static void compositor_present_internal(az_compositor_t *comp, bool recomposited)
{
    unsigned int pitch_px = comp->fb_pitch / 4;

    /*
     * Only a pass that rebuilt the off-screen buffer owes the display
     * anything.  A pointer move does not touch it, so it adds no debt and
     * ends up copying just the two small rectangles the pointer left and
     * landed on — which is what keeps the pointer cheap enough to move at
     * the refresh rate.
     *
     * Whatever is owed goes to *both* buffers: the one about to be drawn
     * into was last painted two frames ago, so this frame's damage alone
     * would leave the previous frame's damage unpaid in it.
     */
    if (recomposited) {
        if (comp->has_damage) {
            int x0 = comp->dirty_min_x < 0 ? 0 : comp->dirty_min_x;
            int y0 = comp->dirty_min_y < 0 ? 0 : comp->dirty_min_y;
            int x1 = comp->dirty_max_x > (int)comp->fb_width  ? (int)comp->fb_width  : comp->dirty_max_x;
            int y1 = comp->dirty_max_y > (int)comp->fb_height ? (int)comp->fb_height : comp->dirty_max_y;
            rect_union(&comp->pending[0], x0, y0, x1, y1);
            rect_union(&comp->pending[1], x0, y0, x1, y1);
        } else {
            /* Nothing said what changed, so assume all of it did. */
            rect_union(&comp->pending[0], 0, 0, (int)comp->fb_width, (int)comp->fb_height);
            rect_union(&comp->pending[1], 0, 0, (int)comp->fb_width, (int)comp->fb_height);
        }
    }

    /* With a hardware cursor overlay the pointer is not in the framebuffer at
     * all, so it owes the copy nothing and is never drawn here. */
    azwm_rect_t new_cursor = comp->hw_cursor
        ? (azwm_rect_t){ 0, 0, 0, 0, 0 }
        : cursor_bounds(comp, comp->cursor_x, comp->cursor_y);

    if (comp->hw_page_flip) {
        int next = 1 - comp->active_vram_buf;
        unsigned int *dst = comp->vram_buf[next];

        /* What this buffer is owed, plus the pointer left in it two frames
         * ago — copying over that is what erases it. */
        azwm_rect_t area = comp->pending[next];
        if (!comp->hw_cursor) {
            rect_union_rect(&area, &comp->cursor_rect[next]);
            rect_union_rect(&area, &new_cursor);
        }

        copy_rect(comp, dst, &area);
        if (!comp->hw_cursor)
            desktop_draw_cursor(dst, comp->fb_width, comp->fb_height, pitch_px,
                                comp->cursor_x, comp->cursor_y, comp->current_cursor_type);

        comp->pending[next].valid = 0;
        comp->cursor_rect[next]   = new_cursor;

        if (display_pan(comp, next) == 0) {
            comp->active_vram_buf = next;
            comp->frontbuf        = dst;
        } else {
            /* The pan failed; this buffer is no longer trustworthy, so give
             * up on flipping rather than showing a stale half of the screen. */
            comp->hw_page_flip = 0;
            comp->frontbuf     = comp->vram_buf[comp->active_vram_buf];
            rect_union(&comp->pending[0], 0, 0, (int)comp->fb_width, (int)comp->fb_height);
            rect_union(&comp->pending[1], 0, 0, (int)comp->fb_width, (int)comp->fb_height);
        }
    } else {
        /* Single-buffered: copy the damage straight to the live scanout. */
        azwm_rect_t area = comp->pending[0];
        if (!comp->hw_cursor) {
            rect_union_rect(&area, &comp->cursor_rect[0]);
            rect_union_rect(&area, &new_cursor);
        }

        copy_rect(comp, comp->frontbuf, &area);
        if (!comp->hw_cursor)
            desktop_draw_cursor(comp->frontbuf, comp->fb_width, comp->fb_height, pitch_px,
                                comp->cursor_x, comp->cursor_y, comp->current_cursor_type);

        /* Tell the kernel exactly what changed so a host-backed framebuffer
         * (VirtIO-GPU) transfers only these rows, not the whole screen. */
        if (comp->fb_fd >= 0 && area.valid && area.x1 > area.x0 && area.y1 > area.y0) {
            struct fb_az_rect d = {
                (unsigned)(area.x0 < 0 ? 0 : area.x0),
                (unsigned)(area.y0 < 0 ? 0 : area.y0),
                (unsigned)(area.x1 - (area.x0 < 0 ? 0 : area.x0)),
                (unsigned)(area.y1 - (area.y0 < 0 ? 0 : area.y0)),
            };
            ioctl(comp->fb_fd, FBIOAZ_DAMAGE, &d);
        }

        comp->pending[0].valid = 0;
        comp->pending[1].valid = 0;
        comp->cursor_rect[0]   = new_cursor;
    }

    comp->has_damage  = 0;
    comp->dirty_min_x = (int)comp->fb_width;
    comp->dirty_min_y = (int)comp->fb_height;
    comp->dirty_max_x = 0;
    comp->dirty_max_y = 0;

    comp->old_cursor_x = comp->cursor_x;
    comp->old_cursor_y = comp->cursor_y;
    comp->frame_count++;

    /* Re-sample current_fps about once a second of wall-clock time. Cheap
     * enough to do on every present (cursor-only ones included, since those
     * are real frames too) — one clock_gettime call and, on 999 out of 1000
     * of them, nothing else. */
    {
        unsigned long long now_ns = (unsigned long long)monotonic_now_ns();
        if (now_ns != 0) {
            if (comp->last_fps_time == 0) {
                comp->last_fps_time        = now_ns;
                comp->last_fps_frame_count = comp->frame_count;
            } else {
                unsigned long long elapsed = now_ns - comp->last_fps_time;
                if (elapsed >= 1000000000ULL) {
                    unsigned long long frames = comp->frame_count - comp->last_fps_frame_count;
                    comp->current_fps = (unsigned int)((frames * 1000000000ULL) / elapsed);
                    comp->last_fps_time        = now_ns;
                    comp->last_fps_frame_count = comp->frame_count;
                }
            }
        }
    }
}

void compositor_present(az_compositor_t *comp)
{
    compositor_present_internal(comp, true);
}

void compose_screen(az_compositor_t *comp)
{
    /*
     * The region compositor_present() is actually going to copy out this
     * frame — the damage box clamped to the screen, or the whole screen when
     * nothing called compositor_damage() (the same "nothing said what
     * changed, assume all of it did" rule compositor_present_internal()
     * applies on the far end). Steps 1 and 2 below use it to skip work whose
     * result could not reach the screen anyway, on the same has_damage
     * contract the present step already trusts.
     */
    int clip_x0 = 0, clip_y0 = 0;
    int clip_x1 = (int)comp->fb_width, clip_y1 = (int)comp->fb_height;
    if (comp->has_damage) {
        clip_x0 = comp->dirty_min_x < 0 ? 0 : comp->dirty_min_x;
        clip_y0 = comp->dirty_min_y < 0 ? 0 : comp->dirty_min_y;
        clip_x1 = comp->dirty_max_x > (int)comp->fb_width  ? (int)comp->fb_width  : comp->dirty_max_x;
        clip_y1 = comp->dirty_max_y > (int)comp->fb_height ? (int)comp->fb_height : comp->dirty_max_y;

        /* blur_backdrop_region() reads whatever is already in the backbuf
         * under a blurred window and trusts it to be this frame's real
         * composite of everything behind it. That only holds if every
         * window below it actually redrew this frame — which a narrow
         * damage box can skip for a window outside it. Rather than track
         * which windows a blur window overlaps, widen to the whole screen
         * whenever one is visible at all: rare (typically just the lock
         * screen), so paying for a full recomposite on its account is
         * cheap insurance against blurring an already-blurred frame. */
        for (unsigned int wi = 0; wi < AZWM_MAX_WINDOWS; wi++) {
            az_window_t *bw = &comp->window_pool[wi];
            if (bw->wid != 0 && bw->visible && bw->blur_backdrop) {
                clip_x0 = 0; clip_y0 = 0;
                clip_x1 = (int)comp->fb_width;
                clip_y1 = (int)comp->fb_height;
                break;
            }
        }
    }

    /* ── 1. Clear backbuf only if tail window does not fully cover screen,
     *      and only the part of it the damage box actually names ───────── */
    az_window_t *tail = comp->list_tail;
    bool tail_has_frame = win_has_frame(tail);
    bool full_coverage = (tail && tail->visible && tail->wid != 0 && !tail_has_frame &&
                          tail->x <= 0 && tail->y <= 0 &&
                          tail->width >= comp->fb_width && tail->height >= comp->fb_height);
    if (!full_coverage && clip_x1 > clip_x0 && clip_y1 > clip_y0) {
        bb_fill_rect(comp, clip_x0, clip_y0, clip_x1 - clip_x0, clip_y1 - clip_y0, 0xFF1E1E2E);
    }

    /* ── 2. Draw windows that can touch the clip (back to front in Z-order) ─
     * Every window still gets a full, un-clipped repaint when it is drawn —
     * only *whether* it's worth drawing at all is decided here — except its
     * client-area blit, which render_window itself narrows to the clip
     * (see the comment on render_window). */
    az_window_t *curr = comp->list_tail;
    while (curr) {
        if (curr->visible && curr->wid != 0) {
            bool has_frame = win_has_frame(curr);
            int ox = curr->x, oy = curr->y;
            int ow = (int)curr->width, oh = (int)curr->height;
            if (has_frame) {
                ox -= AZWM_BORDER_W;
                oy -= (AZWM_TITLEBAR_H + AZWM_BORDER_W);
                ow += 2 * AZWM_BORDER_W;
                oh += (AZWM_TITLEBAR_H + 2 * AZWM_BORDER_W);
            }
            /* Padded by more than the drop shadow's 12px radius so a
             * focused window's shadow is never left stale just outside its
             * own frame. */
            if (ox - 14 < clip_x1 && ox + ow + 14 > clip_x0 &&
                oy - 14 < clip_y1 && oy + oh + 14 > clip_y0) {
                render_window(comp, curr, clip_x0, clip_y0, clip_x1, clip_y1);
            }
        }
        curr = curr->prev;
    }

    /* ── 3. Draw Snapping Preview, Alt+Tab HUD & Context Menu Overlays ─── */
    draw_snap_preview(comp, clip_x0, clip_y0, clip_x1, clip_y1);
    draw_alt_tab_hud(comp, clip_x0, clip_y0, clip_x1, clip_y1);
    draw_context_menu(comp, clip_x0, clip_y0, clip_x1, clip_y1);
    draw_fps_hud(comp, clip_x0, clip_y0, clip_x1, clip_y1);

    /*
     * ── 4. Presentation ───────────────────────────────────────────────
     * Everything above is redrawn from scratch, but only the regions the
     * damage box names actually come out different — the rest re-renders to
     * identical pixels.  Presenting that box is therefore both correct and
     * far cheaper than the screen.  A pass that reports no damage at all is
     * the exception and is presented whole.
     */
    compositor_present_internal(comp, true);
}

/*
 * A pointer move changes almost nothing, so it presents the same way a full
 * frame does — the damage bookkeeping already narrows the copy to the two
 * small rectangles the pointer left and landed on.
 */
void compositor_update_cursor(az_compositor_t *comp)
{
    /* A hardware overlay moves with one message and no framebuffer touch —
     * no recomposite, no damage copy, no transfer to the host. */
    if (comp->hw_cursor) {
        struct fb_az_hwcursor_pos p = { comp->cursor_x, comp->cursor_y };
        ioctl(comp->hw_cursor_fd, FBIOAZ_HWCURSOR_MOVE, &p);
        comp->old_cursor_x = comp->cursor_x;
        comp->old_cursor_y = comp->cursor_y;
        return;
    }
    compositor_present_internal(comp, false);
}

/* ── Animation Engine ────────────────────────────────────────────────────── */

void compositor_trigger_open_animation(az_compositor_t *comp, az_window_t *win)
{
    if (!win_has_frame(win)) return;
    win->anim_state    = AZWM_ANIM_OPEN;
    win->anim_start_ns = monotonic_now_ns();
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
    win->anim_start_ns = monotonic_now_ns();
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
    win->anim_start_ns = monotonic_now_ns();
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

/*
 * compositor_animate_step() drives progress from elapsed wall-clock time
 * (anim_start_ns), not a fixed count of calls. It used to advance one fixed
 * step per call and finish after AZWM_ANIM_STEPS calls — which only gives a
 * fixed real-world duration if this function is called at a fixed rate, and
 * it isn't: the main loop below calls it on every spin of an unthrottled
 * while(running) whenever anything is animating, with no sleep in that
 * branch at all. So the animation's actual on-screen duration was however
 * long the scheduler let this process spin through AZWM_ANIM_STEPS
 * iterations — a handful of *microseconds* on an otherwise-idle machine
 * (i.e. no visible animation at all, just a pop), and something else
 * entirely under load. Timing it against monotonic_now_ns() instead makes
 * every open/minimize/restore take the same AZWM_ANIM_DURATION_NS on any
 * machine, at any system load, regardless of how many times this function
 * happens to get called along the way — which is also what makes it safe
 * to pair with the frame-rate cap in main() below without the animation's
 * apparent speed changing when that cap changes how often we're called.
 */
int compositor_animate_step(az_compositor_t *comp)
{
    int still_animating = 0;
    /* Distinct from still_animating: true for the settling call too, the one
     * where a window's last leg of motion lands exactly on its target and
     * anim_state drops back to NONE. still_animating is false on that call
     * (correctly — there is nothing left to step next time), but the window
     * moved this call and that final frame still needs to reach the screen;
     * returning still_animating there used to tell the caller "nothing to
     * redraw" and skip compositing it, so every animated window visibly
     * stopped one eased step short of its real final geometry until
     * something unrelated forced the next redraw. */
    int any_window_updated = 0;
    long long now_ns = monotonic_now_ns();
    az_window_t *curr = comp->list_head;
    while (curr) {
        if (curr->anim_state != AZWM_ANIM_NONE) {
            any_window_updated = 1;
            long long elapsed = now_ns - curr->anim_start_ns;
            if (elapsed < 0) elapsed = 0; /* clock_gettime hiccup: hold at start rather than misfire */

            /* Ease-out quadratic: progress = t * (512 - t) / 256 */
            int t = (int)((elapsed * 256) / AZWM_ANIM_DURATION_NS);
            if (t > 256) t = 256;
            int ease = (t * (512 - t)) / 256;

            curr->x = curr->anim_start_x + ((curr->anim_target_x - curr->anim_start_x) * ease) / 256;
            curr->y = curr->anim_start_y + ((curr->anim_target_y - curr->anim_start_y) * ease) / 256;
            curr->width = (unsigned int)((int)curr->anim_start_w + ((int)(curr->anim_target_w - curr->anim_start_w) * ease) / 256);
            curr->height = (unsigned int)((int)curr->anim_start_h + ((int)(curr->anim_target_h - curr->anim_start_h) * ease) / 256);

            if (elapsed >= AZWM_ANIM_DURATION_NS) {
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
                curr->anim_start_ns = 0;
            } else {
                still_animating = 1;
            }
        }
        curr = curr->next;
    }
    comp->has_animating_windows = still_animating;
    return any_window_updated;
}

/* ── Cursor, Opacity, and Title API Helpers ──────────────────────────────── */

void compositor_set_cursor(az_compositor_t *comp, unsigned int cursor_type)
{
    if (!comp) return;
    if (cursor_type >= AZ_CURSOR_COUNT) cursor_type = AZ_CURSOR_DEFAULT;
    if (comp->current_cursor_type != cursor_type) {
        comp->current_cursor_type = cursor_type;
        compositor_damage(comp, comp->cursor_x - 8, comp->cursor_y - 8,
                          DESKTOP_CURSOR_W + 16, DESKTOP_CURSOR_H + 16);
    }
}

void compositor_set_window_opacity(az_compositor_t *comp, az_window_t *win, unsigned char opacity)
{
    if (!comp || !win) return;
    if (win->opacity != opacity) {
        win->opacity = opacity;
        compositor_damage(comp, win->x - AZWM_BORDER_W - 16, win->y - AZWM_TITLEBAR_H - AZWM_BORDER_W - 16,
                          (int)win->width + 2 * AZWM_BORDER_W + 32, (int)win->height + AZWM_TITLEBAR_H + 2 * AZWM_BORDER_W + 32);
    }
}

void compositor_set_window_title(az_compositor_t *comp, az_window_t *win, const char *title)
{
    if (!comp || !win) return;
    int ti = 0;
    if (title) {
        while (title[ti] && ti < 63) { win->title[ti] = title[ti]; ti++; }
    }
    win->title[ti] = '\0';
    compositor_damage(comp, win->x - AZWM_BORDER_W, win->y - AZWM_TITLEBAR_H - AZWM_BORDER_W,
                      (int)win->width + 2 * AZWM_BORDER_W, AZWM_TITLEBAR_H + AZWM_BORDER_W);
}

void compositor_set_window_pinned(az_compositor_t *comp, az_window_t *win, unsigned char pinned)
{
    if (!comp || !win) return;
    win->pinned = pinned ? 1 : 0;
    compositor_damage(comp, win->x - AZWM_BORDER_W, win->y - AZWM_TITLEBAR_H - AZWM_BORDER_W,
                      (int)win->width + 2 * AZWM_BORDER_W, AZWM_TITLEBAR_H + AZWM_BORDER_W);
}

