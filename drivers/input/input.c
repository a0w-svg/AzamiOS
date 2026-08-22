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

static void queue_push(const input_event_t *evt)
{
    irqflags_t irqf = spinlock_lock_irqsave(&g_input_lock);
    u32 next = (g_queue_head + 1) % INPUT_QUEUE_SIZE;
    if (next == g_queue_tail) {
        /* Queue full — drop oldest event */
        g_queue_tail = (g_queue_tail + 1) % INPUT_QUEUE_SIZE;
    }
    g_queue[g_queue_head] = *evt;
    g_queue_head = next;
    spinlock_unlock_irqrestore(&g_input_lock, irqf);
}

/* ── PS/2 controller helpers ─────────────────────────────────────────────── */
static void ps2_wait_write(void)
{
    for (int i = 0; i < 500000; i++) {
        if (!(inb(0x64) & 0x02)) return;
        __asm__ volatile("pause");
    }
}

static void ps2_wait_read(void)
{
    for (int i = 0; i < 500000; i++) {
        if (inb(0x64) & 0x01) return;
        __asm__ volatile("pause");
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


/* ── Keyboard IRQ handler (IRQ1 = vector 33) ────────────────────────────────── */
static void keyboard_irq_handler(pt_regs_t *r, void *ctx)
{
    (void)r; (void)ctx;
    /* NOTE: EOI is sent by isr_dispatch() via hal_irq_eoi() after this
     * handler returns.  Do NOT call lapic_eoi() here. */

    irqflags_t irqf = spinlock_lock_irqsave(&g_ps2_lock);
    u8 status = inb(0x64);
    if (!(status & 0x01) || (status & 0x20)) {
        spinlock_unlock_irqrestore(&g_ps2_lock, irqf);
        return;
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
        return;
    }

    if (scancode == 0xE1) {
        g_e1_state = 1;
        return;
    }

    /* Handle E0 prefix */
    if (scancode == 0xE0) {
        g_e0_state = 1;
        return;
    }

    bool released = (scancode & 0x80) != 0;
    u8   code     = scancode & 0x7F;
    u16  keycode  = 0;

    if (g_e0_state) {
        g_e0_state = 0;
        keycode = g_scancode_e0[code];
        if (!keycode) return; /* Ignore unmapped or fake shifts */

        /* Map specific extended modifiers */
        if (keycode == KEY_RCTRL) {
            g_ctrl_held = !released;
        } else if (keycode == KEY_RALT) {
            g_alt_held = !released;
        }
    } else {
        /* Base scancode processing */
        keycode = g_shift_held ? g_scancode_shift[code] : g_scancode_base[code];

        /* Update standard modifiers */
        if (keycode == KEY_LSHIFT || keycode == KEY_RSHIFT) {
            g_shift_held = !released;
            return;
        }
        if (keycode == KEY_LCTRL) {
            g_ctrl_held = !released;
            return;
        }
        if (keycode == KEY_LALT) {
            g_alt_held = !released;
            return;
        }

        /* Update toggles on PRESSED */
        if (!released) {
            bool leds_changed = false;
            if (keycode == KEY_CAPSLOCK)   { g_capslock   = !g_capslock;   leds_changed = true; }
            if (keycode == KEY_NUMLOCK)    { g_numlock    = !g_numlock;    leds_changed = true; }
            if (keycode == KEY_SCROLLLOCK) { g_scrolllock = !g_scrolllock; leds_changed = true; }
            if (leds_changed) keyboard_update_leds_unlocked();
        }

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

    if (!keycode) return;

    /* Build event */
    input_event_t evt;
    __builtin_memset(&evt, 0, sizeof(evt));
    evt.type     = INPUT_EVENT_KEY;
    evt.scancode = scancode;
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
    /* EOI is handled by isr_dispatch() after this function returns */
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

    while (1) {
        irqflags_t irqf = spinlock_lock_irqsave(&g_ps2_lock);
        u8 status = inb(0x64);
        if (!(status & 0x01) || !(status & 0x20)) {
            /* No more mouse data in the output buffer */
            spinlock_unlock_irqrestore(&g_ps2_lock, irqf);
            break;
        }

        u8 data = inb(0x60);
        spinlock_unlock_irqrestore(&g_ps2_lock, irqf);

        cpu_info_t *cpu = smp_get_cpu();
        u32 now_ticks = cpu ? (u32)cpu->ticks : 0;

        /* If > 10 ticks (100ms) elapsed mid-packet, reset to byte 0 to prevent permanent desync */
        if (g_mouse_cycle != 0 && (now_ticks - g_last_mouse_tick) > 10) {
            g_mouse_cycle = 0;
        }
        g_last_mouse_tick = now_ticks;

        /* Discard out-of-sync packets (bit 3 must be 1, and overflow bits 6/7 must be 0 for byte 0) */
        if (g_mouse_cycle == 0 && (!(data & 0x08) || (data & 0xC0))) {
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

            /* Sign extend 9-bit delta values */
            if (!(flags & 0x40)) { /* Not X overflow */
                if (flags & 0x10) {
                    dx = (s16)((u16)g_mouse_bytes[1] | 0xFF00);
                } else {
                    dx = (s16)(g_mouse_bytes[1] & 0x00FF);
                }
            }

            if (!(flags & 0x80)) { /* Not Y overflow */
                if (flags & 0x20) {
                    dy = (s16)((u16)g_mouse_bytes[2] | 0xFF00);
                } else {
                    dy = (s16)(g_mouse_bytes[2] & 0x00FF);
                }
            }

            /* PS/2 mouse Y is inverted (moving up is positive in PS/2, negative in screen space) */
            dy = -dy;

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

    /* Flush any stale bytes */
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

    /* Set sample rate (200 Hz for ultra-smooth tracking), resolution, and enable data reporting */
    ps2_mouse_write(0xF3); ps2_mouse_write(200); /* 200 Hz Sample rate */
    ps2_mouse_write(0xE8); ps2_mouse_write(3);   /* Resolution */
    ps2_mouse_write(0xF4);                       /* Enable data reporting */

    /* Flush output buffer to start with clean state */
    for (int i = 0; i < 32; i++) {
        if (inb(0x64) & 0x01) { inb(0x60); }
        else { break; }
    }
}

/* ── Extern: IRQ handler registration from idt.c ─────────────────────────── */
extern void idt_register_irq(u8 vector, void (*fn)(pt_regs_t *, void *), void *ctx);

/* ── Public API ──────────────────────────────────────────────────────────── */

int input_poll(input_event_t *out);

static s64 input_fops_read(struct file *filp, void *buf, size_t len, u64 *offset)
{
    (void)filp; (void)offset;
    if (len < sizeof(input_event_t)) return -(s64)EINVAL;
    
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

void input_init(void)
{
    /* Register keyboard handler on IRQ1 (vector 33) */
    idt_register_irq(33, keyboard_irq_handler, NULL);

    /* Initialize PS/2 mouse and register handler on IRQ12 (vector 44) */
    mouse_init();
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
    irqflags_t irqf = spinlock_lock_irqsave(&g_input_lock);
    u32 h = g_queue_head;
    u32 t = g_queue_tail;
    u32 count = (h >= t) ? (h - t) : (INPUT_QUEUE_SIZE - t + h);
    spinlock_unlock_irqrestore(&g_input_lock, irqf);
    return count;
}
