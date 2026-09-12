/* ============================================================================
 * AzamiOS — Display Server Protocol Definition
 * File: user/apps/azwm/protocol.h
 *
 * Defines the IPC message types exchanged between GUI client applications
 * (via libazgfx) and the Azami Window Manager (azwm).
 * All messages fit within IPC_MSG_MAX_SIZE (256 bytes).
 * ============================================================================ */
#pragma once

/* ── Window Server IPC Message Types ──────────────────────────────────────── */
enum az_wm_msg_type {
    /* Client → Server */
    AZ_WM_CREATE_WINDOW    = 1,    /* Request: create a new window */
    AZ_WM_DESTROY_WINDOW   = 2,    /* Request: destroy a window */
    AZ_WM_INVALIDATE       = 4,    /* Notify: window content changed */
    AZ_WM_MOVE_WINDOW      = 20,   /* Request: move window to (x, y) */

    /* Server → Client */
    AZ_WM_WINDOW_CREATED   = 3,    /* Response: window created successfully */
    AZ_WM_KEY_EVENT        = 10,   /* Forwarded: keyboard event to focused window */
    AZ_WM_MOUSE_EVENT      = 11,   /* Forwarded: mouse event */
    AZ_WM_FOCUS_CHANGE     = 21,   /* Notify: focus gained/lost */
    AZ_WM_RESTORE_WINDOW   = 22,   /* Request: restore & focus minimized window */
    AZ_WM_MINIMIZE_WINDOW  = 23,   /* Request: minimize window */
    AZ_WM_WINDOW_RESIZED   = 24,   /* Notify: window's content surface was reallocated */
    AZ_WM_SET_OPACITY      = 25,   /* Request: set window alpha opacity (0..255) */
    AZ_WM_SET_TITLE        = 26,   /* Request: update window title dynamically */
    AZ_WM_SET_CURSOR       = 27,   /* Request: set active cursor shape */
    AZ_WM_SET_PINNED       = 28,   /* Request: toggle always-on-top */
};

/* ── Standard Cursor Types ────────────────────────────────────────────────── */
#define AZ_CURSOR_DEFAULT       0   /* Classic modern arrow pointer */
#define AZ_CURSOR_POINTER       1   /* Hand pointing finger (buttons, links) */
#define AZ_CURSOR_IBEAM         2   /* Text I-Beam (text inputs, editors) */
#define AZ_CURSOR_CROSSHAIR     3   /* Precision reticle (canvas, paint) */
#define AZ_CURSOR_MOVE          4   /* 4-way move cross (window dragging) */
#define AZ_CURSOR_RESIZE_NWSE   5   /* Diagonal resize (\) */
#define AZ_CURSOR_RESIZE_NESW   6   /* Diagonal resize (/) */
#define AZ_CURSOR_RESIZE_EW     7   /* Horizontal resize (<->) */
#define AZ_CURSOR_RESIZE_NS     8   /* Vertical resize (up-down) */
#define AZ_CURSOR_WAIT          9   /* Spinner / busy indicator */
#define AZ_CURSOR_COUNT         10

/* ── Window creation flags (msg.create.flags) ─────────────────────────────── */
#define AZ_WIN_FLAG_BLUR_BACKDROP  (1u << 0)
#define AZ_WIN_FLAG_TRANSLUCENT    (1u << 1)

/* ── Standard Keycode Definitions (matches drivers/input/input.h) ────────── */
#ifndef KEY_ESC
#define KEY_ESC          27
#define KEY_BACKSPACE    '\b'
#define KEY_TAB          '\t'
#define KEY_ENTER        '\n'

#define KEY_F1           128
#define KEY_F2           129
#define KEY_F3           130
#define KEY_F4           131
#define KEY_F5           132
#define KEY_F6           133
#define KEY_F7           134
#define KEY_F8           135
#define KEY_F9           136
#define KEY_F10          137
#define KEY_F11          138
#define KEY_F12          139

#define KEY_UP           140
#define KEY_DOWN         141
#define KEY_LEFT         142
#define KEY_RIGHT        143

#define KEY_INSERT       144
#define KEY_DELETE       145
#define KEY_HOME         146
#define KEY_END          147
#define KEY_PAGEUP       148
#define KEY_PAGEDOWN     149

#define KEY_CAPSLOCK     150
#define KEY_NUMLOCK      151
#define KEY_SCROLLLOCK   152

#define KEY_LSHIFT       153
#define KEY_RSHIFT       154
#define KEY_LCTRL        155
#define KEY_RCTRL        156
#define KEY_LALT         157
#define KEY_RALT         158

#define KEY_LSUPER       159
#define KEY_RSUPER       160
#define KEY_APPS         161

#define KEY_PRINTSCREEN  162
#define KEY_PAUSE        163
#endif

/* ── Key Modifier Flags (msg.key.modifiers) ──────────────────────────────── */
#define AZ_MOD_SHIFT     0x0001
#define AZ_MOD_CTRL      0x0002
#define AZ_MOD_ALT       0x0004
#define AZ_MOD_CAPS      0x0008
#define AZ_MOD_NUM       0x0010

/* ── Window Server Message (fits in ipc_msg_t.data[256]) ──────────────────── */
typedef struct {
    unsigned int type;         /* az_wm_msg_type */
    unsigned int wid;          /* Window ID (0 if not applicable) */
    unsigned int client_chan;  /* Client's reply channel ID */
    unsigned int _reserved;

    union {
        /* AZ_WM_CREATE_WINDOW: client → server */
        struct {
            int x, y;
            unsigned int w, h;
            char title[64];
            unsigned int flags;  /* AZ_WIN_FLAG_* above; 0 for a plain window */
        } create;

        /* AZ_WM_WINDOW_CREATED: server → client */
        struct {
            unsigned int shmem_id;
            unsigned int assigned_wid;
            unsigned int width;
            unsigned int height;
        } created;

        /* AZ_WM_KEY_EVENT: server → client */
        struct {
            unsigned char keycode;
            unsigned char pressed;
            unsigned short modifiers;
            unsigned char scancode;
            unsigned char _pad[3];
        } key;

        /*
         * AZ_WM_MOUSE_EVENT: server → client
         *
         * dx/dy are pointer motion, wheel is the scroll wheel — they are not
         * the same thing and a client that scrolls must read `wheel`, or
         * simply moving the pointer across it will scroll its content.
         * `wheel` is positive scrolling down, matching the PS/2 Z delta.
         *
         * The wheel field replaces padding, so the payload is the same size
         * it always was.
         */
        struct {
            short dx, dy;
            short abs_x, abs_y;
            short wheel;
            unsigned char buttons;
            unsigned char _pad[1];
        } mouse;

        /* AZ_WM_MOVE_WINDOW: client → server */
        struct {
            int x, y;
        } move;

        /*
         * AZ_WM_INVALIDATE: client → server.
         *
         * (x, y, w, h) is the changed rectangle, in the client's own window-
         * local coordinates (0,0 = top-left of its content area) — the
         * server adds the window's screen position itself. w == 0 && h == 0
         * means "the whole client area", which is what every sender that
         * predates this field means too: az_wm_msg_t is always zeroed
         * before the type/wid fields are set (see uk_invalidate() and the
         * few call sites that build this message by hand), so an old
         * binary and a client that never calls uk_invalidate_rect() both
         * land on that same fallback for free — see AZ_WM_INVALIDATE's
         * handler in azwm's main loop for the compositor-side half of this.
         */
        struct {
            int x, y;
            unsigned int w, h;
        } invalidate;

        /* AZ_WM_FOCUS_CHANGE: server → client */
        struct {
            unsigned char focused;  /* 1 = gained, 0 = lost */
        } focus;

        /* AZ_WM_WINDOW_RESIZED: server → client */
        struct {
            unsigned int shmem_id;  /* New surface — remap in place of the old one */
            unsigned int width;
            unsigned int height;
        } resized;

        /* AZ_WM_SET_OPACITY: client → server */
        struct {
            unsigned char opacity;  /* 0 = transparent, 255 = fully opaque */
            unsigned char _pad[3];
        } opacity;

        /* AZ_WM_SET_TITLE: client → server */
        struct {
            char title[64];
        } set_title;

        /* AZ_WM_SET_CURSOR: client → server */
        struct {
            unsigned int cursor;    /* AZ_CURSOR_* */
        } set_cursor;

        /* AZ_WM_SET_PINNED: client → server */
        struct {
            unsigned char pinned;   /* 1 = always on top, 0 = normal */
            unsigned char _pad[3];
        } pinned;

        /* Generic padding to ensure struct matches 272-byte ipc_msg_t size */
        unsigned char _raw[256];
    };
} az_wm_msg_t;
