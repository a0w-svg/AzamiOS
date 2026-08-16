/* ============================================================================
 * AzamiOS — Input Subsystem (PS/2 Keyboard + Mouse)
 * File: drivers/input/input.h
 *
 * Provides a unified event queue for keyboard and mouse input, consumed by
 * user-space programs via the SYS_AZ_INPUT_POLL syscall.
 * ============================================================================ */
#pragma once

#include "../../include/azami/types.h"
#include "../../include/azami/defs.h"

/* ── Event types ─────────────────────────────────────────────────────────── */
#define INPUT_EVENT_NONE     0
#define INPUT_EVENT_KEY      1   /* Keyboard key press / release */
#define INPUT_EVENT_MOUSE    2   /* Mouse movement or button */

/* ── Key event flags ─────────────────────────────────────────────────────── */
#define KEY_FLAG_PRESSED     0x0001
#define KEY_FLAG_RELEASED    0x0002
#define KEY_FLAG_SHIFT       0x0004
#define KEY_FLAG_CTRL        0x0008
#define KEY_FLAG_ALT         0x0010
#define KEY_FLAG_CAPS_LOCK   0x0020
#define KEY_FLAG_NUM_LOCK    0x0040
#define KEY_FLAG_SCROLL_LOCK 0x0080

/* ── Special Keycodes (128-255) ──────────────────────────────────────────── */
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

#define KEY_NUMPAD_ENTER 164
#define KEY_NUMPAD_DIV   165
#define KEY_NUMPAD_MUL   166
#define KEY_NUMPAD_SUB   167
#define KEY_NUMPAD_ADD   168
#define KEY_NUMPAD_5     169

/* ── Mouse button bits ───────────────────────────────────────────────────── */
#define MOUSE_BTN_LEFT     0x01
#define MOUSE_BTN_RIGHT    0x02
#define MOUSE_BTN_MIDDLE   0x04
#define MOUSE_BTN_4        0x08
#define MOUSE_BTN_5        0x10

/* ── Input event structure (16 bytes, fits nicely in arrays) ─────────────── */
typedef struct {
    u8   type;          /* INPUT_EVENT_KEY or INPUT_EVENT_MOUSE */
    u8   keycode;       /* Translated ASCII keycode (for KEY events) */
    u16  flags;         /* KEY_FLAG_* or 0 */
    s16  mouse_dx;      /* Mouse delta X (for MOUSE events) */
    s16  mouse_dy;      /* Mouse delta Y (for MOUSE events) */
    u8   mouse_buttons; /* MOUSE_BTN_* bitmask */
    u8   scancode;      /* Raw PS/2 scancode */
    s8   mouse_dz;      /* Mouse scroll wheel delta */
    u8   _pad[1];       /* Alignment padding */
    u32  timestamp;     /* Tick counter at event time */
} __packed input_event_t;

BUILD_ASSERT(sizeof(input_event_t) == 16, "input_event_t must be 16 bytes");

/* ── Public API ──────────────────────────────────────────────────────────── */

/** input_init() — Register PS/2 keyboard and mouse IRQ handlers. */
void input_init(void);

/** input_register_devfs() — Register input devices to devfs. */
void input_register_devfs(void);

/**
 * input_poll(out) — Dequeue one input event (non-blocking).
 * Returns 0 on success with event written to *out, or -1 if queue is empty.
 */
int input_poll(input_event_t *out);

/**
 * input_queue_count() — Number of pending events in the input queue.
 */
u32 input_queue_count(void);
