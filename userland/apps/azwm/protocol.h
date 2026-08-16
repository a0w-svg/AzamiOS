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
};

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

        /* AZ_WM_MOUSE_EVENT: server → client */
        struct {
            short dx, dy;
            short abs_x, abs_y;
            unsigned char buttons;
            unsigned char _pad[3];
        } mouse;

        /* AZ_WM_MOVE_WINDOW: client → server */
        struct {
            int x, y;
        } move;

        /* AZ_WM_FOCUS_CHANGE: server → client */
        struct {
            unsigned char focused;  /* 1 = gained, 0 = lost */
        } focus;

        /* Generic padding to ensure union is large enough */
        unsigned char _raw[200];
    };
} az_wm_msg_t;
