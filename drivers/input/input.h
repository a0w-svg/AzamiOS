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
#define INPUT_EVENT_MOUSE_ABS 3  /* Absolute mouse coordinates */

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

/* ── evdev-compatible numbering ──────────────────────────────────────────────
 * The live key-state bitmap (input_get_key_state()) and the scancode remap
 * table (input_get/set_scancode_keymap()) are both indexed in the same space
 * evdev's EVIOCGBIT(EV_KEY) already advertises to userspace: 1..88 are the AT
 * set-1 scancodes verbatim (evdev.c's evt.scancode & 0x7F), and the five
 * mouse buttons sit at Linux's BTN_LEFT..BTN_EXTRA (0x110..0x114).  Keeping
 * one numbering shared between input.c and evdev.c means there is only ever
 * one bitmap to keep in sync, never two. */
#define INPUT_KEY_CNT      0x300   /* Linux KEY_CNT: one bit per EV_KEY code */
#define INPUT_BTN_LEFT     0x110
#define INPUT_BTN_RIGHT    0x111
#define INPUT_BTN_MIDDLE   0x112
#define INPUT_BTN_SIDE     0x113
#define INPUT_BTN_EXTRA    0x114

/* ── LED identifiers for input_set_led() ─────────────────────────────────── */
#define INPUT_LED_CAPSLOCK    0
#define INPUT_LED_NUMLOCK     1
#define INPUT_LED_SCROLLLOCK  2

/* ── Input event structure (16 bytes, fits nicely in arrays) ─────────────── */
typedef struct {
    u8   type;          /* INPUT_EVENT_KEY, MOUSE, or MOUSE_ABS */
    u8   keycode;       /* Translated ASCII keycode (for KEY events) */
    u16  flags;         /* KEY_FLAG_* or 0 */
    s16  mouse_dx;      /* Mouse delta X (or absolute X) */
    s16  mouse_dy;      /* Mouse delta Y (or absolute Y) */
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

/**
 * input_inject(evt) — Queue an event from a non-PS/2 input driver.
 *
 * Lets bus-attached input hardware (virtio-input, and later USB HID) feed the
 * same queue the PS/2 controller does, so consumers never learn which device
 * an event came from.
 */
void input_inject(const input_event_t *evt);

/**
 * input_inject_scancode(code, pressed, extended) — queue a key from a
 * non-PS/2 keyboard using the shared keymap.
 *
 * @code is an AT set-1 scancode with the release bit stripped, and @extended
 * selects the 0xE0-prefixed block.  Routing through here rather than through
 * input_inject() means modifier state, Caps/Num Lock and the shift keymap all
 * behave identically to the PS/2 keyboard.
 */
void input_inject_scancode(u8 code, bool pressed, bool extended);

/**
 * input_register_poll_source(fn) — Register a drain callback.
 *
 * Devices without a usable interrupt line hand the subsystem a function that
 * pulls pending hardware events into the queue; it runs at the head of every
 * input_poll() and input_queue_count(), so a polled device feels the same as
 * an interrupt-driven one to userspace.
 *
 * Returns 0 on success, negative if the source table is full.
 */
int input_register_poll_source(void (*fn)(void));

/**
 * input_register_observer(fn) — tee every event to another consumer.
 *
 * The legacy queue is drained destructively by input_poll(), so a second
 * reader — the evdev character device, say — cannot simply poll it without
 * stealing events from the first.  Observers instead see every event as it is
 * queued, and keep their own buffering.
 *
 * @fn runs with the input subsystem's lock held, and from interrupt context:
 * it must not block, and must not call back into the input subsystem.
 *
 * Returns 0 on success, negative if the observer table is full.
 */
int input_register_observer(void (*fn)(const input_event_t *));

/**
 * input_get_key_state(bitmap, len) — copy the live per-key/button bitmap.
 *
 * Bit N is set for exactly as long as the key or mouse button carrying evdev
 * code N is held down (see the "evdev-compatible numbering" block above), so
 * evdev's EVIOCGKEY can hand it to userspace unmodified.  The bitmap is
 * updated centrally in queue_push(), the one place every event — PS/2,
 * virtio-input, injected — passes through before any consumer sees it, so
 * this and evdev's own view of the world can never drift apart.
 *
 * Copies min(len, internal bitmap size) bytes into @bitmap and zero-fills any
 * remainder; returns -1 only if @bitmap is NULL.
 */
int input_get_key_state(u8 *bitmap, size_t len);

/**
 * input_set_led(led, on) — force one keyboard lock LED (and its tracked
 * boolean) to a state chosen by userspace.
 *
 * @led is one of INPUT_LED_*.  This updates the same g_capslock/g_numlock/
 * g_scrolllock booleans a real key press would toggle and reprograms the
 * PS/2 LEDs through the existing keyboard_update_leds() path, so a write(2)
 * of an EV_LED record (evdev.c) and an actual Caps Lock keypress compose
 * instead of fighting over which one is "true": whichever happened last
 * wins, exactly as on real Linux.
 */
void input_set_led(u32 led, bool on);

/**
 * input_set_keyboard_repeat(delay_ms, period_ms) — reprogram PS/2 typematic
 * rate/delay (command 0xF3).
 *
 * The requested values are rounded to the nearest hardware-supported step
 * (delay: 250/500/750/1000 ms; period: the 32-entry non-linear rate table
 * every PS/2 keyboard controller implements) and the byte is sent the same
 * ACK-waiting way mouse_init() already talks to the auxiliary port.  On
 * success the *actual* applied values are stored, so a later
 * input_get_keyboard_repeat() call — and EVIOCGREP — report truth instead of
 * the raw request.  Returns false if the keyboard never ACKs.
 */
bool input_set_keyboard_repeat(u32 delay_ms, u32 period_ms);

/** input_get_keyboard_repeat(out_delay, out_period) — last applied values. */
void input_get_keyboard_repeat(u32 *delay_ms, u32 *period_ms);

/**
 * input_get_scancode_keymap(scancode) — the keycode a scancode currently
 * produces.
 *
 * @scancode is flat: 0..127 for the unprefixed block, 0x80 | code for the
 * 0xE0-prefixed block — the same split EVIOCSKEYCODE below uses. Returns
 * whatever override is installed, or the built-in keymap entry if none is.
 */
u16 input_get_scancode_keymap(u16 scancode);

/**
 * input_set_scancode_keymap(scancode, keycode) — install a per-key remap.
 *
 * Overrides the built-in AT set-1 keymap for one scancode, checked by
 * keyboard_emit() before it falls back to g_scancode_base/shift/e0. A
 * @keycode of 0 clears the override rather than mapping the key to nothing —
 * this keymap has no separate "explicitly disabled" state, matching the
 * common case (remapping, not disabling) that EVIOCSKEYCODE is for.
 */
void input_set_scancode_keymap(u16 scancode, u16 keycode);
