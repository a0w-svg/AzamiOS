/* ============================================================================
 * AzamiOS — Input Subsystem Implementation (PS/2 Keyboard + Mouse)
 * File: drivers/input.c
 *
 * Handles IRQ1 (keyboard) and IRQ12 (mouse) from the legacy 8259 PIC.
 * Events are queued into a lock-free ring buffer and consumed by user-space
 * processes via the SYS_AZ_INPUT_POLL syscall.
 *
 * Keyboard: PS/2 Scan Code Set 1 → ASCII translation (US QWERTY layout).
 * Mouse:    Standard PS/2 3-byte protocol (dx, dy, buttons).
 * ============================================================================ */

#define DEBUG 1
#include <azami/debug.h>
#include "input.h"
#include "../char/console.h"
#include "../../arch/x86_64/cpu/idt.h"
#include "../../hal/irq.h"
#include "../../arch/x86_64/cpu/smp.h"
#include "../../arch/x86_64/cpu/spinlock.h"
#include "../../arch/x86_64/cpu/hwaccel.h"
#include "../../include/azami/defs.h"
#include "../../fs/vfs.h"
#include "../../kernel/syscall/syscall.h" /* For EFAULT, EINVAL */
#include "../../kernel/uaccess.h" /* For copy_to_user */

extern int devfs_register_device(const char *name, file_operations_t *fops, void *private_data);


/* ── Ring buffer for input events ─────────────────────────────────────────── */
#define INPUT_QUEUE_SIZE  128
static input_event_t g_queue[INPUT_QUEUE_SIZE];
static volatile u32  g_queue_head = 0;   /* Next slot to write */
static volatile u32  g_queue_tail = 0;   /* Next slot to read */
static spinlock_t    g_input_lock = SPINLOCK_INIT;
static spinlock_t    g_ps2_lock   = SPINLOCK_INIT;

/* Consumers that see events without removing them from the queue. */
#define INPUT_MAX_OBSERVERS 4
static void (*g_observers[INPUT_MAX_OBSERVERS])(const input_event_t *);
static u32    g_observer_count = 0;

int input_register_observer(void (*fn)(const input_event_t *))
{
    if (!fn) return -1;
    if (g_observer_count >= INPUT_MAX_OBSERVERS) return -1;
    g_observers[g_observer_count++] = fn;
    return 0;
}

/* ── Live per-key/button state bitmap (backs EVIOCGKEY) ──────────────────────
 * One bit per evdev EV_KEY code (see the "evdev-compatible numbering" block
 * in input.h). Updated centrally in queue_push(), the single choke point
 * every event flows through before any consumer — the legacy queue, evdev —
 * sees it, so this can never drift from what evdev itself reports. */
static u8 g_key_state[INPUT_KEY_CNT / 8];

static inline void key_state_set(u32 bit, bool on)
{
    if (bit / 8 >= sizeof(g_key_state)) return;
    if (on) g_key_state[bit / 8] |= (u8)(1u << (bit % 8));
    else    g_key_state[bit / 8] &= (u8)~(1u << (bit % 8));
}

int input_get_key_state(u8 *bitmap, size_t len)
{
    if (!bitmap) return -1;

    irqflags_t irqf = spinlock_lock_irqsave(&g_input_lock);
    size_t n = (len < sizeof(g_key_state)) ? len : sizeof(g_key_state);
    __builtin_memcpy(bitmap, g_key_state, n);
    spinlock_unlock_irqrestore(&g_input_lock, irqf);

    if (n < len) __builtin_memset(bitmap + n, 0, len - n);
    return 0;
}

/* Mouse-button bit -> evdev BTN_* code, in the same order evdev.c's own
 * press/release translation uses. Unlike g_key_state this is a fixed
 * protocol mapping, not state, so keeping a second copy carries no risk of
 * drift. */
static const struct { u8 mask; u16 code; } g_btn_map[] = {
    { MOUSE_BTN_LEFT,   INPUT_BTN_LEFT   },
    { MOUSE_BTN_RIGHT,  INPUT_BTN_RIGHT  },
    { MOUSE_BTN_MIDDLE, INPUT_BTN_MIDDLE },
    { MOUSE_BTN_4,      INPUT_BTN_SIDE   },
    { MOUSE_BTN_5,      INPUT_BTN_EXTRA  },
};

/* Caller holds g_input_lock. */
static void key_state_observe(const input_event_t *evt)
{
    if (evt->type == INPUT_EVENT_KEY) {
        u8 sc = evt->scancode & 0x7F;
        if (sc) key_state_set(sc, (evt->flags & KEY_FLAG_RELEASED) == 0);
    } else if (evt->type == INPUT_EVENT_MOUSE) {
        for (u32 i = 0; i < ARRAY_SIZE(g_btn_map); i++) {
            key_state_set(g_btn_map[i].code, (evt->mouse_buttons & g_btn_map[i].mask) != 0);
        }
    }
}

static void queue_push(const input_event_t *evt)
{
    for (u32 i = 0; i < g_observer_count; i++) g_observers[i](evt);

    irqflags_t irqf = spinlock_lock_irqsave(&g_input_lock);
    key_state_observe(evt);
    u32 next = (g_queue_head + 1) % INPUT_QUEUE_SIZE;
    if (next == g_queue_tail) {
        /* Queue full — drop oldest event */
        g_queue_tail = (g_queue_tail + 1) % INPUT_QUEUE_SIZE;
    }
    g_queue[g_queue_head] = *evt;
    g_queue_head = next;
    spinlock_unlock_irqrestore(&g_input_lock, irqf);
}

/* ── Non-PS/2 input sources ──────────────────────────────────────────────── */
/*
 * Drivers on enumerable buses either interrupt or, when their interrupt line
 * is shared and cannot be claimed safely, register a drain callback that runs
 * whenever userspace asks for events.
 */
#define INPUT_MAX_POLL_SOURCES 4
static void (*g_poll_sources[INPUT_MAX_POLL_SOURCES])(void);
static u32    g_poll_source_count = 0;

void input_inject(const input_event_t *evt)
{
    if (evt) queue_push(evt);
}

int input_register_poll_source(void (*fn)(void))
{
    if (!fn) return -1;
    if (g_poll_source_count >= INPUT_MAX_POLL_SOURCES) return -1;
    g_poll_sources[g_poll_source_count++] = fn;
    return 0;
}

/* Never called with g_input_lock held: a source pushes into the same queue. */
static void input_drain_sources(void)
{
    for (u32 i = 0; i < g_poll_source_count; i++) {
        g_poll_sources[i]();
    }
}

/* ── PS/2 controller helpers ─────────────────────────────────────────────── */
static void ps2_wait_write(void)
{
    for (u32 i = 0; i < 500000; i++) {
        if (!(inb(0x64) & 0x02)) return;
        hw_spin_wait(i);
    }
}

static void ps2_wait_read(void)
{
    for (u32 i = 0; i < 500000; i++) {
        if (inb(0x64) & 0x01) return;
        hw_spin_wait(i);
    }
}

static void ps2_write_cmd(u8 cmd)
{
    irqflags_t irqf = spinlock_lock_irqsave(&g_ps2_lock);
    ps2_wait_write();
    outb(0x64, cmd);
    spinlock_unlock_irqrestore(&g_ps2_lock, irqf);
}

static void ps2_write_data(u8 data)
{
    irqflags_t irqf = spinlock_lock_irqsave(&g_ps2_lock);
    ps2_wait_write();
    outb(0x60, data);
    spinlock_unlock_irqrestore(&g_ps2_lock, irqf);
}

static u8 ps2_read_data(void)
{
    irqflags_t irqf = spinlock_lock_irqsave(&g_ps2_lock);
    ps2_wait_read();
    u8 data = inb(0x60);
    spinlock_unlock_irqrestore(&g_ps2_lock, irqf);
    return data;
}

/* ── PS/2 Scan Code Set 1 → Keycodes ─────────────────────────────────────── */
static const u16 g_scancode_base[128] = {
    0, KEY_ESC, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', KEY_BACKSPACE,
    KEY_TAB, 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', KEY_ENTER,
    KEY_LCTRL, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    KEY_LSHIFT, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', KEY_RSHIFT,
    KEY_NUMPAD_MUL, KEY_LALT, ' ', KEY_CAPSLOCK,
    KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F8, KEY_F9, KEY_F10,
    KEY_NUMLOCK, KEY_SCROLLLOCK,
    KEY_HOME, KEY_UP, KEY_PAGEUP, KEY_NUMPAD_SUB,
    KEY_LEFT, KEY_NUMPAD_5, KEY_RIGHT, KEY_NUMPAD_ADD,
    KEY_END, KEY_DOWN, KEY_PAGEDOWN, KEY_INSERT, KEY_DELETE,
    0, 0, 0, KEY_F11, KEY_F12,
};

static const u16 g_scancode_shift[128] = {
    0, KEY_ESC, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', KEY_BACKSPACE,
    KEY_TAB, 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', KEY_ENTER,
    KEY_LCTRL, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    KEY_LSHIFT, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', KEY_RSHIFT,
    KEY_NUMPAD_MUL, KEY_LALT, ' ', KEY_CAPSLOCK,
    KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F8, KEY_F9, KEY_F10,
    KEY_NUMLOCK, KEY_SCROLLLOCK,
    KEY_HOME, KEY_UP, KEY_PAGEUP, KEY_NUMPAD_SUB,
    KEY_LEFT, KEY_NUMPAD_5, KEY_RIGHT, KEY_NUMPAD_ADD,
    KEY_END, KEY_DOWN, KEY_PAGEDOWN, KEY_INSERT, KEY_DELETE,
    0, 0, 0, KEY_F11, KEY_F12,
};

static const u16 g_scancode_e0[128] = {
    [0x1C] = KEY_NUMPAD_ENTER,
    [0x1D] = KEY_RCTRL,
    [0x2A] = 0, /* Fake LShift */
    [0x35] = KEY_NUMPAD_DIV,
    [0x36] = 0, /* Fake RShift */
    [0x37] = KEY_PRINTSCREEN,
    [0x38] = KEY_RALT,
    [0x47] = KEY_HOME,
    [0x48] = KEY_UP,
    [0x49] = KEY_PAGEUP,
    [0x4B] = KEY_LEFT,
    [0x4D] = KEY_RIGHT,
    [0x4F] = KEY_END,
    [0x50] = KEY_DOWN,
    [0x51] = KEY_PAGEDOWN,
    [0x52] = KEY_INSERT,
    [0x53] = KEY_DELETE,
    [0x5B] = KEY_LSUPER,
    [0x5C] = KEY_RSUPER,
    [0x5D] = KEY_APPS,
};

/*
 * Per-key remap overlay (backs EVIOCGKEYCODE/EVIOCSKEYCODE).
 *
 * g_scancode_base/shift/e0 above are `static const` — baked-in defaults, not
 * something a keymap tool can rewrite.  This table sits in front of them:
 * index 0..127 is the unprefixed block, 128..255 is 0xE0-prefixed (flagged by
 * the top bit, mirroring how atkbd folds the same distinction into one flat
 * space on real Linux). A zero entry means "no override" — see
 * input_set_scancode_keymap()'s doc comment for why 0 can't also mean
 * "mapped to nothing".
 */
#define SCANCODE_OVERRIDE_SIZE 256
static u16 g_scancode_override[SCANCODE_OVERRIDE_SIZE];

static inline u16 scancode_flat_index(u8 code, bool extended)
{
    return (u16)(extended ? (0x80 | code) : code);
}

u16 input_get_scancode_keymap(u16 scancode)
{
    if (scancode >= SCANCODE_OVERRIDE_SIZE) return 0;
    if (g_scancode_override[scancode]) return g_scancode_override[scancode];

    u8 code = scancode & 0x7F;
    return (scancode & 0x80) ? g_scancode_e0[code] : g_scancode_base[code];
}

void input_set_scancode_keymap(u16 scancode, u16 keycode)
{
    if (scancode >= SCANCODE_OVERRIDE_SIZE) return;
    g_scancode_override[scancode] = keycode;
}

/* Modifier & State Tracking */
static volatile bool g_shift_held = false;
static volatile bool g_ctrl_held  = false;
static volatile bool g_alt_held   = false;
static volatile bool g_capslock   = false;
static volatile bool g_numlock    = false;
static volatile bool g_scrolllock = false;

/* Multi-byte Tracking */
static u8 g_e0_state = 0;
static u8 g_e1_state = 0;

static void keyboard_update_leds_unlocked(void)
{
    u8 leds = 0;
    if (g_scrolllock) leds |= 1;
    if (g_numlock)    leds |= 2;
    if (g_capslock)   leds |= 4;

    /* Write 0xED to keyboard data port to update LEDs */
    ps2_wait_write();
    outb(0x60, 0xED);
    ps2_wait_read();
    if (inb(0x60) != 0xFA) {
        return; /* No ACK received */
    }

    ps2_wait_write();
    outb(0x60, leds);
    ps2_wait_read();
    inb(0x60); /* ACK */
}

static void keyboard_update_leds(void)
{
    irqflags_t irqf = spinlock_lock_irqsave(&g_ps2_lock);
    keyboard_update_leds_unlocked();
    spinlock_unlock_irqrestore(&g_ps2_lock, irqf);
}

/*
 * input_set_led() — force one lock LED to a userspace-chosen state.
 *
 * evdev's write(2) path (evdev.c) calls this for an EV_LED record instead of
 * touching hardware directly, so this is the only place that needs to know
 * both "which tracked boolean" and "which PS/2 LED bit" a lock corresponds
 * to.  Writing g_capslock/g_numlock/g_scrolllock here — the same booleans
 * keyboard_emit()'s toggle handling flips on a real keypress — means the two
 * paths compose: whichever last touched the lock wins, and a subsequent real
 * keypress toggles from that state rather than from stale hardware state.
 */
void input_set_led(u32 led, bool on)
{
    switch (led) {
    case INPUT_LED_CAPSLOCK:   g_capslock   = on; break;
    case INPUT_LED_NUMLOCK:    g_numlock    = on; break;
    case INPUT_LED_SCROLLLOCK: g_scrolllock = on; break;
    default: return;
    }
    keyboard_update_leds();
}


/*
 * keyboard_emit() — translate one AT set-1 key event and queue it.
 *
 * Shared by the PS/2 handler and by bus-attached keyboards whose keycodes are
 * set-1 compatible, so every keyboard goes through the same keymap, the same
 * modifier tracking and the same Caps/Num Lock rules.
 *
 * @code:        scancode with the release bit already stripped
 * @extended:    true for the 0xE0-prefixed key block
 * @raw_scancode: value reported to userspace in evt.scancode
 * @from_ps2:    only a real PS/2 keyboard gets its lock LEDs reprogrammed
 */
static void keyboard_emit(u8 code, bool released, bool extended,
                          u8 raw_scancode, bool from_ps2)
{
    u16 keycode = 0;
    u16 override = g_scancode_override[scancode_flat_index(code, extended)];

    if (extended) {
        keycode = override ? override : g_scancode_e0[code];
        if (!keycode) return; /* Ignore unmapped or fake shifts */

        /* Map specific extended modifiers */
        if (keycode == KEY_RCTRL) {
            g_ctrl_held = !released;
        } else if (keycode == KEY_RALT) {
            g_alt_held = !released;
        }
    } else {
        /* Base scancode processing */
        keycode = override ? override : (g_shift_held ? g_scancode_shift[code] : g_scancode_base[code]);

        /* Update standard modifiers */
        if (keycode == KEY_LSHIFT || keycode == KEY_RSHIFT) {
            g_shift_held = !released;
        } else if (keycode == KEY_LCTRL) {
            g_ctrl_held = !released;
        } else if (keycode == KEY_LALT) {
            g_alt_held = !released;
        }

        /* Update toggles on PRESSED */
        if (!released) {
            bool leds_changed = false;
            if (keycode == KEY_CAPSLOCK)   { g_capslock   = !g_capslock;   leds_changed = true; }
            if (keycode == KEY_NUMLOCK)    { g_numlock    = !g_numlock;    leds_changed = true; }
            if (keycode == KEY_SCROLLLOCK) { g_scrolllock = !g_scrolllock; leds_changed = true; }
            if (leds_changed && from_ps2) keyboard_update_leds_unlocked();
        }

        /* An overridden key stands entirely on its own — no case-shifting or
         * Num Lock reinterpretation, since those rules exist to reinterpret
         * *this physical position* on the assumption it is still a letter or
         * numpad digit, which a remap has explicitly said it no longer is. */
        if (override) goto build_event;

        /* Apply Caps Lock on alphabetic characters */
        bool is_alpha = (keycode >= 'a' && keycode <= 'z') || (keycode >= 'A' && keycode <= 'Z');
        if (is_alpha) {
            bool upper = g_capslock ^ g_shift_held;
            keycode = upper ? (g_scancode_base[code] - 'a' + 'A') : g_scancode_base[code];
        }

        /* Apply Num Lock on Numpad keys */
        if (code >= 0x47 && code <= 0x53 && code != 0x4A && code != 0x4E) {
            bool use_num = g_numlock ^ g_shift_held;
            if (use_num) {
                if      (code == 0x47) keycode = '7';
                else if (code == 0x48) keycode = '8';
                else if (code == 0x49) keycode = '9';
                else if (code == 0x4B) keycode = '4';
                else if (code == 0x4C) keycode = '5';
                else if (code == 0x4D) keycode = '6';
                else if (code == 0x4F) keycode = '1';
                else if (code == 0x50) keycode = '2';
                else if (code == 0x51) keycode = '3';
                else if (code == 0x52) keycode = '0';
                else if (code == 0x53) keycode = '.';
            }
        }
    }

build_event:
    if (!keycode) return;

    /* Build event */
    input_event_t evt;
    __builtin_memset(&evt, 0, sizeof(evt));
    evt.type     = INPUT_EVENT_KEY;
    evt.scancode = raw_scancode;
    evt.keycode  = keycode;
    evt.flags    = released ? KEY_FLAG_RELEASED : KEY_FLAG_PRESSED;

    if (g_shift_held)  evt.flags |= KEY_FLAG_SHIFT;
    if (g_ctrl_held)   evt.flags |= KEY_FLAG_CTRL;
    if (g_alt_held)    evt.flags |= KEY_FLAG_ALT;
    if (g_capslock)    evt.flags |= KEY_FLAG_CAPS_LOCK;
    if (g_numlock)     evt.flags |= KEY_FLAG_NUM_LOCK;
    if (g_scrolllock)  evt.flags |= KEY_FLAG_SCROLL_LOCK;

    cpu_info_t *cpu = smp_get_cpu();
    evt.timestamp = cpu ? (u32)cpu->ticks : 0;

    queue_push(&evt);
}

void input_inject_scancode(u8 code, bool pressed, bool extended)
{
    keyboard_emit(code & 0x7F, !pressed, extended,
                  (u8)((code & 0x7F) | (pressed ? 0x00 : 0x80)), false);
}

/* ── Keyboard IRQ handler (IRQ1 = vector 33) ────────────────────────────────── */
/*
 * Upper bound on bytes drained from the 8042 in one interrupt.
 *
 * Both handlers below loop until the controller says its output buffer is
 * empty. That termination depends on the controller answering truthfully —
 * and when no controller is fitted, or the chipset leaves the port floating,
 * inb(0x64) reads 0xFF: "data ready" (bit 0) *and* "from the mouse" (bit 5)
 * are both set, and every read of 0x60 returns 0xFF again. The mouse handler
 * then never met its exit condition and spun forever inside the interrupt.
 *
 * A real burst is a handful of bytes at PS/2 signalling rates, so this bound
 * is never reached in normal operation; it exists so that a dead or absent
 * controller costs one bounded interrupt instead of the CPU.
 */
#define PS2_ISR_MAX_BYTES 256u

/* 0xFF from the status port means nothing is driving the bus. */
#define PS2_STATUS_FLOATING 0xFFu

static void keyboard_irq_handler(pt_regs_t *r, void *ctx)
{
    (void)r; (void)ctx;
    /* NOTE: EOI is sent by isr_dispatch() via hal_irq_eoi() after this
     * handler returns.  Do NOT call lapic_eoi() here. */

    for (u32 guard = PS2_ISR_MAX_BYTES; guard; guard--) {
        irqflags_t irqf = spinlock_lock_irqsave(&g_ps2_lock);
        u8 status = inb(0x64);
        if (status == PS2_STATUS_FLOATING ||
            !(status & 0x01) || (status & 0x20)) {
            spinlock_unlock_irqrestore(&g_ps2_lock, irqf);
            break;
        }
        u8 scancode = inb(0x60);
        spinlock_unlock_irqrestore(&g_ps2_lock, irqf);

        /* Handle E1 (Pause) state machine */
        if (g_e1_state > 0) {
            g_e1_state++;
            if (g_e1_state == 6) {
                g_e1_state = 0;

                input_event_t evt;
                __builtin_memset(&evt, 0, sizeof(evt));
                evt.type     = INPUT_EVENT_KEY;
                evt.scancode = 0xE1; /* Virtual scancode for pause */
                evt.flags    = KEY_FLAG_PRESSED;
                evt.keycode  = KEY_PAUSE;

                cpu_info_t *cpu = smp_get_cpu();
                evt.timestamp = cpu ? (u32)cpu->ticks : 0;

                queue_push(&evt);
            }
            continue;
        }

        if (scancode == 0xE1) {
            g_e1_state = 1;
            continue;
        }

        /* Handle E0 prefix */
        if (scancode == 0xE0) {
            g_e0_state = 1;
            continue;
        }

        bool extended = g_e0_state != 0;
        g_e0_state = 0;
        keyboard_emit(scancode & 0x7F, (scancode & 0x80) != 0, extended, scancode, true);
    }
}

/* ── Mouse state machine ─────────────────────────────────────────────────── */
static u8  g_mouse_cycle = 0;
static u8  g_mouse_bytes[4];
static u8  g_mouse_packet_size = 3;
static u8  g_mouse_id = 0;
static s32 g_mouse_x = 0;   /* Absolute cursor X (tracked for user space) */
static s32 g_mouse_y = 0;   /* Absolute cursor Y */

static u8 ps2_mouse_write(u8 data)
{
    ps2_write_cmd(0xD4);
    ps2_write_data(data);
    return ps2_read_data(); /* ACK */
}

/* ── Mouse IRQ handler (IRQ12 = vector 44) ────────────────────────────────── */
static u32 g_last_mouse_tick = 0;

static void mouse_irq_handler(pt_regs_t *r, void *ctx)
{
    (void)r; (void)ctx;
    /* NOTE: EOI is sent by isr_dispatch() via hal_irq_eoi() after this
     * handler returns. Do NOT call lapic_eoi() here. */

    for (u32 guard = PS2_ISR_MAX_BYTES; guard; guard--) {
        irqflags_t irqf = spinlock_lock_irqsave(&g_ps2_lock);
        u8 status = inb(0x64);
        /* 0xFF is an absent controller, not a mouse byte — and it satisfies
         * both of the conditions below, which is what used to wedge this
         * loop. Check it first. */
        if (status == PS2_STATUS_FLOATING ||
            !(status & 0x01) || !(status & 0x20)) {
            /* No more mouse data in the output buffer */
            spinlock_unlock_irqrestore(&g_ps2_lock, irqf);
            break;
        }

        u8 data = inb(0x60);
        spinlock_unlock_irqrestore(&g_ps2_lock, irqf);

        cpu_info_t *cpu = smp_get_cpu();
        u32 now_ticks = cpu ? (u32)cpu->ticks : 0;

        /* If > 10 ticks (100ms) elapsed mid-packet, reset to byte 0 to recover from dropped bytes */
        if (g_mouse_cycle != 0 && (now_ticks - g_last_mouse_tick) > 10) {
            g_mouse_cycle = 0;
        }
        g_last_mouse_tick = now_ticks;

        /* Discard out-of-sync packets: Byte 0 must have bit 3 set to 1 */
        if (g_mouse_cycle == 0 && !(data & 0x08)) {
            continue;
        }

        g_mouse_bytes[g_mouse_cycle] = data;
        g_mouse_cycle = (g_mouse_cycle + 1) % g_mouse_packet_size;

        if (g_mouse_cycle == 0) {
            /* Full packet received */
            u8  flags   = g_mouse_bytes[0];
            s16 dx      = 0;
            s16 dy      = 0;
            s8  dz      = 0;
            u8  buttons = flags & 0x07;

            /* Sign extend 9-bit delta values with overflow clamping */
            int raw_x = (int)(int8_t)g_mouse_bytes[1];
            int raw_y = (int)(int8_t)g_mouse_bytes[2];

            if (flags & 0x10) raw_x |= ~0xFF;
            else raw_x &= 0xFF;

            if (flags & 0x20) raw_y |= ~0xFF;
            else raw_y &= 0xFF;

            if (flags & 0x40) raw_x = (flags & 0x10) ? -255 : 255;
            if (flags & 0x80) raw_y = (flags & 0x20) ? -255 : 255;

            dx = (s16)raw_x;
            dy = -(s16)raw_y; /* PS/2 Y is inverted (up is positive in PS/2, negative on screen) */

            /* Handle 4th byte for IntelliMouse / Explorer extensions */
            if (g_mouse_packet_size == 4) {
                u8 b4 = g_mouse_bytes[3];
                if (g_mouse_id == 3) {
                    dz = (s8)b4;
                } else if (g_mouse_id == 4) {
                    u8 z = b4 & 0x0F;
                    if (z & 0x08) dz = (s8)(z | 0xF0);
                    else dz = (s8)z;
                    if (b4 & 0x10) buttons |= MOUSE_BTN_4;
                    if (b4 & 0x20) buttons |= MOUSE_BTN_5;
                }
            }

            /* Update absolute position */
            g_mouse_x += dx;
            g_mouse_y += dy;
            if (g_mouse_x < 0)    g_mouse_x = 0;
            if (g_mouse_y < 0)    g_mouse_y = 0;
            if (g_mouse_x > 4095) g_mouse_x = 4095;
            if (g_mouse_y > 4095) g_mouse_y = 4095;

            input_event_t evt;
            __builtin_memset(&evt, 0, sizeof(evt));
            evt.type          = INPUT_EVENT_MOUSE;
            evt.mouse_dx      = dx;
            evt.mouse_dy      = dy;
            evt.mouse_dz      = dz;
            evt.mouse_buttons = buttons;
            evt.timestamp     = now_ticks;

            queue_push(&evt);
        }
    }
    /* EOI is handled by isr_dispatch() after this function returns */
}

static void mouse_init(void)
{
    /* Enable the auxiliary (mouse) PS/2 port */
    ps2_write_cmd(0xA8);

    /* Tell the controller to enable IRQ12 and IRQ1 */
    ps2_write_cmd(0x20);         /* Read current config */
    u8 config = ps2_read_data();
    config |= 0x02;              /* Enable IRQ12 (mouse) */
    config |= 0x01;              /* Enable IRQ1 (keyboard) */
    config &= ~0x20;             /* Enable mouse clock (clear inhibit bit) */
    config &= ~0x10;             /* Enable keyboard clock (clear inhibit bit) */
    ps2_write_cmd(0x60);         /* Write config back */
    ps2_write_data(config);

    /* Flush any stale bytes before initialization */
    for (int i = 0; i < 32; i++) {
        if (inb(0x64) & 0x01) { inb(0x60); }
        else { break; }
    }

    /* Set defaults (0xF6) */
    ps2_mouse_write(0xF6);
    
    /* Try to enable IntelliMouse (Scroll Wheel) */
    ps2_mouse_write(0xF3); ps2_mouse_write(200);
    ps2_mouse_write(0xF3); ps2_mouse_write(100);
    ps2_mouse_write(0xF3); ps2_mouse_write(80);
    
    ps2_mouse_write(0xF2); /* Get ID */
    g_mouse_id = ps2_read_data();
    
    if (g_mouse_id == 3) {
        /* IntelliMouse enabled! Try Explorer (5-button) */
        ps2_mouse_write(0xF3); ps2_mouse_write(200);
        ps2_mouse_write(0xF3); ps2_mouse_write(200);
        ps2_mouse_write(0xF3); ps2_mouse_write(80);
        
        ps2_mouse_write(0xF2); /* Get ID */
        g_mouse_id = ps2_read_data();
    }
    
    if (g_mouse_id == 3 || g_mouse_id == 4) {
        g_mouse_packet_size = 4;
    } else {
        g_mouse_packet_size = 3;
    }

    /* Flush any leftover bytes before enabling data reporting */
    for (int i = 0; i < 32; i++) {
        if (inb(0x64) & 0x01) { inb(0x60); }
        else { break; }
    }

    /* Set sample rate (200 Hz for ultra-smooth tracking), resolution, and enable data reporting */
    ps2_mouse_write(0xF3); ps2_mouse_write(200); /* 200 Hz Sample rate */
    ps2_mouse_write(0xE8); ps2_mouse_write(3);   /* Resolution */
    ps2_mouse_write(0xF4);                       /* Enable data reporting */

    g_mouse_cycle = 0;
}

/* ── Extern: IRQ handler registration from idt.c ─────────────────────────── */

/* ── Public API ──────────────────────────────────────────────────────────── */

int input_poll(input_event_t *out);

static s64 input_fops_read(struct file *filp, void *buf, size_t len, u64 *offset)
{
    (void)filp; (void)offset;
    if (!buf || len < sizeof(input_event_t)) return -(s64)EINVAL;
    
    input_event_t evt;
    if (input_poll(&evt) == 0) {
        __builtin_memcpy(buf, &evt, sizeof(evt));
        return (s64)sizeof(evt);
    }
    
    return -(s64)EAGAIN;
}

static s64 mouse_fops_read(struct file *filp, void *buf, size_t len, u64 *offset)
{
    (void)filp; (void)offset;
    if (!buf || len < 3) return -(s64)EINVAL;
    
    input_event_t evt;
    while (input_poll(&evt) == 0) {
        if (evt.type == INPUT_EVENT_MOUSE) {
            u8 packet[3];
            packet[0] = 0x08 | (evt.mouse_buttons & 0x07);
            if (evt.mouse_dx < 0) packet[0] |= 0x10;
            if (-evt.mouse_dy < 0) packet[0] |= 0x20;
            packet[1] = (u8)(evt.mouse_dx & 0xFF);
            packet[2] = (u8)(-evt.mouse_dy & 0xFF);
            __builtin_memcpy(buf, packet, 3);
            return 3;
        }
    }
    return -(s64)EAGAIN;
}

static file_operations_t input_fops = {
    .read = input_fops_read,
};

static file_operations_t mouse_fops = {
    .read = mouse_fops_read,
};

/*
 * PS/2 typematic rate/delay (command 0xF3) encoding.
 *
 * The parameter byte is 0b0DDRRRRR: bits 6-5 pick one of four delays before
 * autorepeat starts, bits 4-0 pick one of 32 repeat rates from the
 * non-linear table every 8042-compatible keyboard controller implements
 * (real hardware and QEMU's emulation alike) — see any PS/2 keyboard
 * interface reference (e.g. the "Set Typematic Rate/Delay" command in the
 * IBM PS/2 Trackpoint/keyboard technical reference). Rates run from 30.0
 * characters/sec at index 0 down to 2.0 cps at index 31; the table below is
 * those periods rounded to the nearest millisecond.
 */
static const u16 g_typematic_delay_ms[4] = { 250, 500, 750, 1000 };
static const u16 g_typematic_period_ms[32] = {
     33,  37,  42,  46,  50,  54,  58,  62,  67,  75,  83,  92, 100, 109, 116, 125,
    133, 149, 167, 182, 200, 217, 233, 250, 270, 303, 333, 370, 400, 435, 476, 500,
};

static u32 g_repeat_delay_ms  = 250;
static u32 g_repeat_period_ms = 33;

/*
 * keyboard_write_cmd() — send one command byte to the keyboard itself (not
 * the 8042 controller) over port 0x60, and wait for its 0xFA ACK. Mirrors
 * ps2_mouse_write()'s pattern for the auxiliary port; used for both 0xF4
 * (enable scanning) and 0xF3 (set typematic rate/delay).
 */
static bool keyboard_write_cmd(u8 data)
{
    ps2_write_data(data);
    return ps2_read_data() == 0xFA;
}

/* Index into @table whose value is closest to @ms. */
static u32 typematic_nearest(u32 ms, const u16 *table, u32 count)
{
    u32 best = 0, best_diff = 0xFFFFFFFFu;
    for (u32 i = 0; i < count; i++) {
        u32 diff = (ms > table[i]) ? (ms - table[i]) : (table[i] - ms);
        if (diff < best_diff) { best_diff = diff; best = i; }
    }
    return best;
}

bool input_set_keyboard_repeat(u32 delay_ms, u32 period_ms)
{
    u32 di = typematic_nearest(delay_ms, g_typematic_delay_ms, ARRAY_SIZE(g_typematic_delay_ms));
    u32 ri = typematic_nearest(period_ms, g_typematic_period_ms, ARRAY_SIZE(g_typematic_period_ms));
    u8  byte = (u8)((di << 5) | ri);

    if (!keyboard_write_cmd(0xF3)) return false;
    if (!keyboard_write_cmd(byte)) return false;

    /* Store what the hardware actually applied, not the raw request, so
     * EVIOCGREP reports truth. */
    g_repeat_delay_ms  = g_typematic_delay_ms[di];
    g_repeat_period_ms = g_typematic_period_ms[ri];
    return true;
}

void input_get_keyboard_repeat(u32 *delay_ms, u32 *period_ms)
{
    if (delay_ms)  *delay_ms  = g_repeat_delay_ms;
    if (period_ms) *period_ms = g_repeat_period_ms;
}

static void keyboard_init(void)
{
    /* Enable first PS/2 port (keyboard) */
    ps2_write_cmd(0xAE);

    /* Tell keyboard to enable scanning (0xF4) */
    keyboard_write_cmd(0xF4);

    /* Nothing has ever told the hardware a typematic rate, so it runs at
     * whatever it powered on with. Apply Linux's own default (250ms delay,
     * ~33ms period i.e. ~30cps) up front — it happens to land exactly on the
     * fastest hardware-supported step, so no rounding is even visible. */
    input_set_keyboard_repeat(250, 33);
}

void input_init(void)
{
    /* Initialize PS/2 keyboard and mouse hardware */
    keyboard_init();
    mouse_init();

    /* Register keyboard handler on IRQ1 (vector 33) */
    idt_register_irq(33, keyboard_irq_handler, NULL);

    /* Register mouse handler on IRQ12 (vector 44) */
    idt_register_irq(44, mouse_irq_handler, NULL);

    /* Unmask IRQ1 (keyboard) and IRQ12 (mouse) on the PIC */
    hal_irq_enable(1, 33);    /* IRQ1 = keyboard (vector 33) */
    hal_irq_enable(12, 44);   /* IRQ12 = mouse (vector 44) */
    hal_irq_enable(2, 34);    /* IRQ2 = cascade (vector 34) */

    /* Set initial LEDs */
    keyboard_update_leds();

    devfs_register_device("input0", &input_fops, NULL);
    devfs_register_device("event0", &input_fops, NULL);
    devfs_register_device("psaux", &mouse_fops, NULL);
    devfs_register_device("mice", &mouse_fops, NULL);

    pr_debug("[INPUT] PS/2 Keyboard (IRQ1) and Mouse (IRQ12) initialized\n");
}

int input_poll(input_event_t *out)
{
    if (!out) return -1;
    input_drain_sources();

    irqflags_t irqf = spinlock_lock_irqsave(&g_input_lock);
    if (g_queue_tail == g_queue_head) {
        spinlock_unlock_irqrestore(&g_input_lock, irqf);
        return -1;  /* Queue empty */
    }

    *out = g_queue[g_queue_tail];
    g_queue_tail = (g_queue_tail + 1) % INPUT_QUEUE_SIZE;
    spinlock_unlock_irqrestore(&g_input_lock, irqf);
    return 0;
}

u32 input_queue_count(void)
{
    input_drain_sources();

    irqflags_t irqf = spinlock_lock_irqsave(&g_input_lock);
    u32 h = g_queue_head;
    u32 t = g_queue_tail;
    u32 count = (h >= t) ? (h - t) : (INPUT_QUEUE_SIZE - t + h);
    spinlock_unlock_irqrestore(&g_input_lock, irqf);
    return count;
}
