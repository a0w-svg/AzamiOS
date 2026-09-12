/* ============================================================================
 * AzamiOS — Display Server Compositor
 * File: user/apps/azwm/compositor.h
 * ============================================================================ */
#pragma once

#include "protocol.h"

#define AZWM_MAX_WINDOWS   32
#define AZWM_TITLEBAR_H    24
#define AZWM_BORDER_W      2

#define AZWM_ANIM_NONE     0
#define AZWM_ANIM_OPEN     1
#define AZWM_ANIM_MINIMIZE 2
#define AZWM_ANIM_RESTORE  3

/* Window open/minimize/restore animations are timed against wall-clock
 * nanoseconds (anim_start_ns below), not counted in fixed loop iterations —
 * see compositor_animate_step()'s comment for why a step count doesn't work
 * here. 180ms is snappy without being so short it reads as a flicker. */
#define AZWM_ANIM_DURATION_NS (180LL * 1000000LL)

/* A screen-space rectangle; x1/y1 are exclusive.  `valid` is 0 for "empty",
 * which is not the same as a zero-sized rect at the origin. */
typedef struct {
    int x0, y0, x1, y1;
    int valid;
} azwm_rect_t;

/* ── Window descriptor ────────────────────────────────────────────────────── */
typedef struct az_window_t {
    unsigned int   wid;           /* Window ID (1-based, 0 = unused) */
    unsigned int   owner_pid;     /* Owning process PID */
    unsigned int   client_chan;   /* Client's reply channel for events */
    int            x, y;          /* Position on screen */
    unsigned int   width, height; /* Client area dimensions */
    unsigned int   buffer_w, buffer_h; /* Allocated pixel buffer dimensions (for clipping) */
    unsigned int  *pixels;        /* Pointer to client pixel buffer (shared memory) */
    unsigned int   shmem_id;      /* Shared memory ID for pixel buffer */
    unsigned long  shm_bytes;     /* Actual size in bytes allocated for SHM */
    unsigned char  visible;
    unsigned char  focused;
    unsigned char  maximized;     /* Non-zero if window is currently maximized */
    unsigned char  blur_backdrop; /* AZ_WIN_FLAG_BLUR_BACKDROP was set at create time */
    unsigned char  opacity;       /* Window alpha: 0 = transparent, 255 = fully opaque */
    unsigned char  pinned;        /* 1 = always on top, 0 = normal */
    unsigned int   cursor_type;   /* Client-requested cursor shape */
    /* Pre-maximize geometry, restored on un-maximize */
    int            saved_x, saved_y;
    unsigned int   saved_w, saved_h;
    /* Smooth animation state */
    int            anim_state;    /* AZWM_ANIM_* */
    long long      anim_start_ns; /* CLOCK_MONOTONIC timestamp the animation began at */
    int            anim_start_x, anim_start_y;
    unsigned int   anim_start_w, anim_start_h;
    int            anim_target_x, anim_target_y;
    unsigned int   anim_target_w, anim_target_h;
    char           title[64];
    struct az_window_t *next;
    struct az_window_t *prev;
} az_window_t;

/* ── Compositor state ─────────────────────────────────────────────────────── */
typedef struct {
    /* Framebuffer & Hardware Page Flipping */
    unsigned int *frontbuf;   /* Currently displayed buffer */
    unsigned int *backbuf;    /* Off-screen buffer for rendering */
    unsigned int *vram_buf[2];/* VRAM buffer 0 and buffer 1 for hardware flipping */
    int           hw_page_flip; /* 1 when the display can pan between two buffers */
    int           active_vram_buf; /* 0 or 1 — the one currently being scanned out */
    int           fb_fd;        /* /dev/fb0, held open for FBIOPAN_DISPLAY */
    unsigned int  fb_yres;      /* rows in one VRAM buffer */
    unsigned int  fb_width;
    unsigned int  fb_height;
    unsigned int  fb_pitch;   /* In bytes */

    /*
     * With two buffers, the one about to be drawn into was last painted two
     * frames ago, so it is owed everything that changed since — not just this
     * frame's damage.  `pending[i]` accumulates that debt per buffer and
     * `cursor_rect[i]` remembers where the pointer was left in each, so the
     * copy that repays the debt also erases it.
     */
    azwm_rect_t   pending[2];
    azwm_rect_t   cursor_rect[2];

    /* Windows */
    az_window_t   window_pool[AZWM_MAX_WINDOWS];
    az_window_t  *free_list;
    az_window_t  *list_head;
    az_window_t  *list_tail;
    unsigned int  window_count;
    unsigned int  next_wid;
    az_window_t  *focused_window;  /* Pointer to focused window, NULL if none */

    /* Mouse cursor */
    int           cursor_x;
    int           cursor_y;
    int           old_cursor_x;
    int           old_cursor_y;
    unsigned int  current_cursor_type; /* Active cursor shape (AZ_CURSOR_*) */
    int           hw_cursor;      /* 1 when the display drives the pointer overlay */
    int           hw_cursor_fd;   /* /dev/fb0, held open for the cursor ioctls */

    /* Interactive Enhancements: Snap Preview, Alt+Tab Switcher & Desktop Context Menu */
    int           snap_preview_mode; /* 0 = none, 1 = left, 2 = right, 3 = max, 4 = top-left, 5 = top-right, 6 = bot-left, 7 = bot-right */
    int           alt_tab_active;    /* 1 if Alt+Tab HUD is open */
    int           alt_tab_idx;       /* currently selected window index in Alt+Tab */
    unsigned int  alt_tab_wids[AZWM_MAX_WINDOWS];
    int           alt_tab_count;
    int           ctx_menu_active;   /* 1 if desktop right-click menu is open */
    int           ctx_menu_x;
    int           ctx_menu_y;
    int           ctx_menu_hover;

    /* IPC */
    int           server_channel; /* Channel ID for receiving client requests */

    /* Damage tracking / Dirty rect bounding box */
    int           has_damage;
    int           dirty_min_x;
    int           dirty_min_y;
    int           dirty_max_x;
    int           dirty_max_y;

    /* Active animation tracker */
    int           has_animating_windows;

    /* System-wide clipboard ────────────────────────────────────────────────
     * Stored as raw UTF-8 bytes, NUL-terminated.  Any client may set via
     * AZ_WM_CLIPBOARD_SET and read via AZ_WM_CLIPBOARD_GET.              */
    char          clipboard_buf[4096];
    unsigned int  clipboard_len;   /* bytes in clipboard_buf (excl. NUL)  */

    /* Hardware performance tracking. current_fps is sampled roughly once a
     * second from wall-clock time in compositor_present_internal(), not
     * assumed from frame_count alone — a compositor whose damage tracking is
     * doing its job can go long stretches without a redraw, so "frames since
     * last sample" is only meaningful paired with how much time that took. */
    unsigned long long frame_count;
    unsigned int  current_fps;
    unsigned long long last_fps_time;        /* CLOCK_MONOTONIC ns of last sample, 0 = not yet sampled */
    unsigned long long last_fps_frame_count; /* frame_count as of that sample */
    int           fps_hud_visible;           /* 1 = draw the on-screen FPS counter (F12 toggles it) */

    /* Titlebar button hover — which decoration circle (if any) the pointer
     * currently sits over, so render_window() can lift/brighten it instead
     * of every titlebar button being a dead flat circle regardless of the
     * pointer. 0 = none, matching AZWM_BTN_* below; hover_btn_wid pins it to
     * one window so a stale hover can't paint on the wrong one after focus
     * changes or windows close. Also covers the resize grip (AZWM_BTN_RESIZE)
     * even though it isn't a titlebar circle — it's the same "give some
     * visual answer to the pointer" idea, and it needs a home somewhere. */
    unsigned int  hover_btn_wid;
    int           hover_btn;
} az_compositor_t;

#define AZWM_BTN_NONE   0
#define AZWM_BTN_CLOSE  1
#define AZWM_BTN_MIN    2
#define AZWM_BTN_MAX    3
#define AZWM_BTN_RESIZE 4

/* ── API ──────────────────────────────────────────────────────────────────── */

/** compositor_init(comp, fb, backbuf, w, h, pitch, chan) — Initialize compositor state. */
void compositor_init(az_compositor_t *comp,
                     unsigned int *frontbuf,
                     unsigned int *backbuf,
                     unsigned int w, unsigned int h, unsigned int pitch,
                     int server_channel);

/** compositor_damage(comp, x, y, w, h) — Mark a region as dirty. */
void compositor_damage(az_compositor_t *comp, int x, int y, int w, int h);

/** compositor_damage_all(comp) — Mark entire screen as dirty. */
void compositor_damage_all(az_compositor_t *comp);

/** compositor_create_window(comp, ...) — Create a new window and allocate its pixel buffer.
 *  `flags` is the client's requested AZ_WIN_FLAG_* bitset (0 for a plain window). */
int compositor_create_window(az_compositor_t *comp,
                             unsigned int owner_pid,
                             unsigned int client_chan,
                             int x, int y,
                             unsigned int w, unsigned int h,
                             const char *title,
                             unsigned int flags,
                             unsigned int *out_shmem_id);

/** compositor_destroy_window(comp, wid) — Destroy a window by ID. */
void compositor_destroy_window(az_compositor_t *comp, unsigned int wid);

/** compositor_resize_window(comp, win, new_w, new_h, out_shmem_id) —
 *  reallocate a window's content surface to a new (clamped) size, remapped
 *  into azwm's own address space at its existing per-slot VA. Returns 0 and
 *  fills *out_shmem_id on success (a no-op returning the current shmem_id
 *  when the clamped size already matches); returns -1 and leaves the window
 *  with no valid surface on failure, in which case the caller must destroy
 *  it. The caller is responsible for forwarding *out_shmem_id to the client
 *  (AZ_WM_WINDOW_RESIZED) so its own mapping follows — this only resizes
 *  azwm's side. */
int compositor_resize_window(az_compositor_t *comp, az_window_t *win,
                             unsigned int new_w, unsigned int new_h,
                             unsigned int *out_shmem_id);

/** compositor_find_window_at(comp, x, y) — Find topmost window at screen coordinates. */
int compositor_find_window_at(az_compositor_t *comp, int x, int y);

/** compositor_focus_window(comp, win) — Set focus to window. */
void compositor_focus_window(az_compositor_t *comp, az_window_t *win);

/** compose_screen(comp) — Composite all windows and flip to screen. */
void compose_screen(az_compositor_t *comp);

/**
 * compositor_enable_page_flip(comp, fb_fd, vram, yres) — switch to
 * double-buffered presentation.
 *
 * @vram must be a mapping of at least two screens' worth of video memory and
 * @fb_fd the framebuffer it came from, kept open so the compositor can pan.
 * Composition still happens in the off-screen buffer; presenting becomes a
 * copy into the buffer that is not being displayed, followed by a pan at the
 * frame boundary, so nothing half-drawn is ever on screen.
 */
void compositor_enable_page_flip(az_compositor_t *comp, int fb_fd,
                                 unsigned int *vram, unsigned int yres);

/** compositor_present(comp) — put the composed frame on screen. */
void compositor_present(az_compositor_t *comp);

/**
 * compositor_enable_hw_cursor(comp, fb_fd) — hand the pointer sprite to the
 * display's cursor overlay if it has one. On success the compositor stops
 * drawing the pointer into the framebuffer and a pointer move becomes one
 * ioctl instead of a recomposite. A no-op (leaves hw_cursor 0) when the
 * backend has no overlay.
 */
void compositor_enable_hw_cursor(az_compositor_t *comp, int fb_fd);

/** compositor_update_cursor(comp) — Update only the cursor region without full redraw. */
void compositor_update_cursor(az_compositor_t *comp);

/** Window animation triggers */
void compositor_trigger_open_animation(az_compositor_t *comp, az_window_t *win);
void compositor_trigger_minimize_animation(az_compositor_t *comp, az_window_t *win, int dock_x, int dock_y);
void compositor_trigger_restore_animation(az_compositor_t *comp, az_window_t *win, int dock_x, int dock_y);
int  compositor_animate_step(az_compositor_t *comp);

/** Cursor, Opacity, and Title API Helpers */
void compositor_set_cursor(az_compositor_t *comp, unsigned int cursor_type);
void compositor_set_window_opacity(az_compositor_t *comp, az_window_t *win, unsigned char opacity);
void compositor_set_window_title(az_compositor_t *comp, az_window_t *win, const char *title);
void compositor_set_window_pinned(az_compositor_t *comp, az_window_t *win, unsigned char pinned);

/* CLOCK_MONOTONIC in nanoseconds (0 on failure). Exposed so main()'s frame
 * pacing can measure against the same clock compositor.c's own animation
 * timing and FPS counter use, instead of a second reimplementation. */
long long compositor_now_ns(void);

