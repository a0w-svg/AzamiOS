/* ============================================================================
 * AzamiOS — Display Server Compositor
 * File: user/apps/azwm/compositor.h
 * ============================================================================ */
#pragma once

#include "protocol.h"

#define AZWM_MAX_WINDOWS  32
#define AZWM_TITLEBAR_H   24
#define AZWM_BORDER_W     2

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
    unsigned char  visible;
    unsigned char  focused;
    unsigned char  maximized;     /* Non-zero if window is currently maximized */
    unsigned char  _pad;
    /* Pre-maximize geometry, restored on un-maximize */
    int            saved_x, saved_y;
    unsigned int   saved_w, saved_h;
    char           title[64];
    struct az_window_t *next;
    struct az_window_t *prev;
} az_window_t;

/* ── Compositor state ─────────────────────────────────────────────────────── */
typedef struct {
    /* Framebuffer */
    unsigned int *frontbuf;   /* Mapped framebuffer (front buffer) */
    unsigned int *backbuf;    /* Back buffer (shared memory for compositing) */
    unsigned int  fb_width;
    unsigned int  fb_height;
    unsigned int  fb_pitch;   /* In bytes */

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

    /* Interactive Enhancements: Snap Preview & Alt+Tab Switcher */
    int           snap_preview_mode; /* 0 = none, 1 = left, 2 = right, 3 = max */
    int           alt_tab_active;    /* 1 if Alt+Tab HUD is open */
    int           alt_tab_idx;       /* currently selected window index in Alt+Tab */
    unsigned int  alt_tab_wids[AZWM_MAX_WINDOWS];
    int           alt_tab_count;

    /* IPC */
    int           server_channel; /* Channel ID for receiving client requests */
} az_compositor_t;

/* ── API ──────────────────────────────────────────────────────────────────── */

/** compositor_init(comp, fb, backbuf, w, h, pitch, chan) — Initialize compositor state. */
void compositor_init(az_compositor_t *comp,
                     unsigned int *frontbuf,
                     unsigned int *backbuf,
                     unsigned int w, unsigned int h, unsigned int pitch,
                     int server_channel);

/** compositor_create_window(comp, ...) — Create a new window and allocate its pixel buffer. */
int compositor_create_window(az_compositor_t *comp,
                             unsigned int owner_pid,
                             unsigned int client_chan,
                             int x, int y,
                             unsigned int w, unsigned int h,
                             const char *title,
                             unsigned int *out_shmem_id);

/** compositor_destroy_window(comp, wid) — Destroy a window by ID. */
void compositor_destroy_window(az_compositor_t *comp, unsigned int wid);

/** compositor_find_window_at(comp, x, y) — Find topmost window at screen coordinates. */
int compositor_find_window_at(az_compositor_t *comp, int x, int y);

/** compositor_focus_window(comp, win) — Set focus to window. */
void compositor_focus_window(az_compositor_t *comp, az_window_t *win);

/** compose_screen(comp) — Composite all windows and flip to screen. */
void compose_screen(az_compositor_t *comp);

/** compositor_update_cursor(comp) — Update only the cursor region without full redraw. */
void compositor_update_cursor(az_compositor_t *comp);
