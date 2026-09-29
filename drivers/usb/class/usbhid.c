/* ============================================================================
 * AzamiOS — USB HID class driver: keyboards, mice, tablets
 * File: drivers/usb/class/usbhid.c
 *
 * Feeds the same input subsystem as the PS/2 and virtio-input drivers, so
 * nothing above drivers/input can tell which kind of keyboard or mouse an
 * event came from.
 *
 * Keyboards run in the HID boot protocol (SET_PROTOCOL 0): an 8-byte report
 * of modifier bits plus up to six pressed usages, identical on every
 * keyboard. Presses and releases are the difference between consecutive
 * reports, translated to AT set-1 scancodes and injected through the shared
 * keymap. Unlike a PS/2 keyboard, a USB keyboard never repeats a held key —
 * the host is expected to — so the driver runs typematic repeat itself, at
 * the rate EVIOCSREP / the PS/2 setting chose. It also drives the lock LEDs,
 * which the host must set with SET_REPORT.
 *
 * Pointers run in report protocol when their report descriptor parses, which
 * is what makes absolute devices (QEMU's usb-tablet, touchscreens, KVM
 * switches) and 16-bit mice work; boot mice fall back to the fixed 3-byte
 * boot report when it does not.
 * ============================================================================ */

#define DEBUG 1
#include "../../../include/azami/debug.h"
#include "../core/usb.h"
#include "../../input/input.h"
#include "../../../kernel/mm/kmalloc.h"
#include "../../../kernel/lib/string.h"
#include "../../../arch/x86_64/boot/limine_req.h"

#define HID_REQ_GET_REPORT    0x01
#define HID_REQ_SET_REPORT    0x09
#define HID_REQ_SET_IDLE      0x0A
#define HID_REQ_SET_PROTOCOL  0x0B

#define HID_SUBCLASS_BOOT     1
#define HID_PROTO_KEYBOARD    1
#define HID_PROTO_MOUSE       2

#define HID_PROTOCOL_BOOT     0
#define HID_PROTOCOL_REPORT   1

/* Usages this driver understands, as (page << 16) | id. */
#define HID_UP_GENDESK        0x0001
#define HID_UP_BUTTON         0x0009
#define HID_USAGE_X           ((HID_UP_GENDESK << 16) | 0x30)
#define HID_USAGE_Y           ((HID_UP_GENDESK << 16) | 0x31)
#define HID_USAGE_WHEEL       ((HID_UP_GENDESK << 16) | 0x38)

#define HID_MAX_FIELDS        16

typedef struct hid_field {
    u32  usage;
    u8   report_id;
    u16  bit_offset;       /* within the report, after the ID byte */
    u8   bit_size;
    bool relative;
    s32  logical_min, logical_max;
} hid_field_t;

typedef enum { HID_KBD, HID_POINTER } hid_kind_t;

typedef struct usbhid {
    usb_device_t *dev;
    hid_kind_t    kind;
    u8            iface;
    bool          uses_report_ids;

    /* pointer (report protocol) */
    hid_field_t   fields[HID_MAX_FIELDS];
    u32           nfields;
    bool          boot_mouse;          /* fixed boot report instead */
    s32           last_abs_x, last_abs_y;
    bool          have_abs;

    /* keyboard */
    u8            prev[8];
    u8            repeat_usage;        /* 0 = nothing repeating */
    u64           repeat_next_ns;
    u32           leds_shown;          /* INPUT_LED_* bitmask on the device */
    bool          leds_known;
} usbhid_t;

/* ── Keyboard usage → AT set-1 ───────────────────────────────────────────────
 * Low byte: scancode. Bit 8: the key lives in the 0xE0-prefixed block.
 * Zero: no set-1 equivalent (the key is ignored). */
#define E0(x) (0x100 | (x))
static const u16 g_usage_to_set1[0xE8] = {
    [0x04] = 0x1E, [0x05] = 0x30, [0x06] = 0x2E, [0x07] = 0x20, [0x08] = 0x12,
    [0x09] = 0x21, [0x0A] = 0x22, [0x0B] = 0x23, [0x0C] = 0x17, [0x0D] = 0x24,
    [0x0E] = 0x25, [0x0F] = 0x26, [0x10] = 0x32, [0x11] = 0x31, [0x12] = 0x18,
    [0x13] = 0x19, [0x14] = 0x10, [0x15] = 0x13, [0x16] = 0x1F, [0x17] = 0x14,
    [0x18] = 0x16, [0x19] = 0x2F, [0x1A] = 0x11, [0x1B] = 0x2D, [0x1C] = 0x15,
    [0x1D] = 0x2C,
    [0x1E] = 0x02, [0x1F] = 0x03, [0x20] = 0x04, [0x21] = 0x05, [0x22] = 0x06,
    [0x23] = 0x07, [0x24] = 0x08, [0x25] = 0x09, [0x26] = 0x0A, [0x27] = 0x0B,
    [0x28] = 0x1C, [0x29] = 0x01, [0x2A] = 0x0E, [0x2B] = 0x0F, [0x2C] = 0x39,
    [0x2D] = 0x0C, [0x2E] = 0x0D, [0x2F] = 0x1A, [0x30] = 0x1B, [0x31] = 0x2B,
    [0x32] = 0x2B, [0x33] = 0x27, [0x34] = 0x28, [0x35] = 0x29, [0x36] = 0x33,
    [0x37] = 0x34, [0x38] = 0x35, [0x39] = 0x3A,
    [0x3A] = 0x3B, [0x3B] = 0x3C, [0x3C] = 0x3D, [0x3D] = 0x3E, [0x3E] = 0x3F,
    [0x3F] = 0x40, [0x40] = 0x41, [0x41] = 0x42, [0x42] = 0x43, [0x43] = 0x44,
    [0x44] = 0x57, [0x45] = 0x58,
    [0x46] = E0(0x37), [0x47] = 0x46,
    [0x49] = E0(0x52), [0x4A] = E0(0x47), [0x4B] = E0(0x49), [0x4C] = E0(0x53),
    [0x4D] = E0(0x4F), [0x4E] = E0(0x51), [0x4F] = E0(0x4D), [0x50] = E0(0x4B),
    [0x51] = E0(0x50), [0x52] = E0(0x48),
    [0x53] = 0x45, [0x54] = E0(0x35), [0x55] = 0x37, [0x56] = 0x4A, [0x57] = 0x4E,
    [0x58] = E0(0x1C), [0x59] = 0x4F, [0x5A] = 0x50, [0x5B] = 0x51, [0x5C] = 0x4B,
    [0x5D] = 0x4C, [0x5E] = 0x4D, [0x5F] = 0x47, [0x60] = 0x48, [0x61] = 0x49,
    [0x62] = 0x52, [0x63] = 0x53, [0x64] = 0x56, [0x65] = E0(0x5D),
    /* Modifiers, also reported as usages 0xE0..0xE7 by report-protocol
     * keyboards; the boot report carries them as bits 0..7 of byte 0. */
    [0xE0] = 0x1D, [0xE1] = 0x2A, [0xE2] = 0x38, [0xE3] = E0(0x5B),
    [0xE4] = E0(0x1D), [0xE5] = 0x36, [0xE6] = E0(0x38), [0xE7] = E0(0x5C),
};

static void kbd_key(u8 usage, bool pressed)
{
    if (usage >= sizeof(g_usage_to_set1) / sizeof(g_usage_to_set1[0])) return;
    u16 sc = g_usage_to_set1[usage];
    if (!sc) return;
    input_inject_scancode((u8)(sc & 0xFF), pressed, (sc & 0x100) != 0);
}

static bool report_has(const u8 *r, u8 usage)
{
    for (int i = 2; i < 8; i++) if (r[i] == usage) return true;
    return false;
}

static void kbd_report(usbhid_t *h, const u8 *r, u32 len)
{
    if (len < 8) return;
    /* ErrorRollOver: more keys down than the report can describe. The keys
     * array is garbage for this one report; keep the previous state. */
    if (r[2] == 0x01) return;

    u8 mods_changed = (u8)(r[0] ^ h->prev[0]);
    for (int b = 0; b < 8; b++)
        if (mods_changed & (1 << b)) kbd_key((u8)(0xE0 + b), (r[0] >> b) & 1);

    for (int i = 2; i < 8; i++) {
        u8 u = h->prev[i];
        if (u > 3 && !report_has(r, u)) {
            kbd_key(u, false);
            if (u == h->repeat_usage) h->repeat_usage = 0;
        }
    }
    for (int i = 2; i < 8; i++) {
        u8 u = r[i];
        if (u > 3 && !report_has(h->prev, u)) {
            kbd_key(u, true);
            /* The newest key is the one that repeats, as on every keyboard;
             * lock keys never do. */
            if (u != 0x39 && u != 0x47 && u != 0x53) {
                h->repeat_usage   = u;
                h->repeat_next_ns = 0;   /* armed by the poll hook: needs "now" */
            }
        }
    }
    memcpy(h->prev, r, 8);
}

/* ── Report descriptor parser ─────────────────────────────────────────────── */

typedef struct {
    u16 usage_page;
    s32 logical_min, logical_max;
    u8  report_size, report_count, report_id;
} hid_globals_t;

static s32 item_signed(u32 v, u8 size)
{
    if (size == 1) return (s8)v;
    if (size == 2) return (s16)v;
    return (s32)v;
}

/* Walk the descriptor, recording every Input field whose usage this driver
 * uses. Returns the number of recorded fields. */
static u32 hid_parse(usbhid_t *h, const u8 *d, u32 len)
{
    hid_globals_t g = { 0 }, stack[4];
    int sp = 0;
    u32 usages[16], nusages = 0;
    u32 umin = 0, umax = 0;
    bool have_range = false;
    u16 offset[256] = { 0 };   /* running bit offset per report ID */

    for (u32 i = 0; i < len; ) {
        u8 prefix = d[i];
        if (prefix == 0xFE) {                       /* long item: skip */
            if (i + 2 >= len) break;
            i += 3u + d[i + 1];
            continue;
        }
        u8 size = (u8)(prefix & 3);
        if (size == 3) size = 4;
        u8 type = (u8)((prefix >> 2) & 3);
        u8 tag  = (u8)(prefix >> 4);
        if (i + 1 + size > len) break;
        u32 v = 0;
        for (u8 b = 0; b < size; b++) v |= (u32)d[i + 1 + b] << (8 * b);
        i += 1u + size;

        if (type == 1) {                            /* global */
            switch (tag) {
            case 0x0: g.usage_page   = (u16)v; break;
            case 0x1: g.logical_min  = item_signed(v, size); break;
            case 0x2: g.logical_max  = size < 4 && g.logical_min >= 0 ? (s32)v : item_signed(v, size); break;
            case 0x7: g.report_size  = (u8)v; break;
            case 0x8: g.report_id    = (u8)v; h->uses_report_ids = true; break;
            case 0x9: g.report_count = (u8)v; break;
            case 0xA: if (sp < 4) stack[sp++] = g; break;
            case 0xB: if (sp > 0) g = stack[--sp]; break;
            default: break;
            }
        } else if (type == 2) {                     /* local */
            u32 full = size == 4 ? v : ((u32)g.usage_page << 16) | v;
            if (tag == 0x0 && nusages < 16) usages[nusages++] = full;
            else if (tag == 0x1) { umin = full; have_range = true; }
            else if (tag == 0x2) { umax = full; have_range = true; }
        } else if (type == 0) {                     /* main */
            if (tag == 0x8) {                       /* Input */
                bool constant = v & 1, variable = v & 2, relative = v & 4;
                for (u32 k = 0; k < g.report_count; k++) {
                    u32 usage = 0;
                    if (nusages) usage = usages[k < nusages ? k : nusages - 1];
                    else if (have_range && umin + k <= umax) usage = umin + k;
                    bool wanted = !constant && variable &&
                                  (usage == HID_USAGE_X || usage == HID_USAGE_Y ||
                                   usage == HID_USAGE_WHEEL ||
                                   (usage >> 16) == HID_UP_BUTTON);
                    if (wanted && h->nfields < HID_MAX_FIELDS && g.report_size <= 32) {
                        hid_field_t *f = &h->fields[h->nfields++];
                        f->usage       = usage;
                        f->report_id   = g.report_id;
                        f->bit_offset  = offset[g.report_id];
                        f->bit_size    = g.report_size;
                        f->relative    = relative;
                        f->logical_min = g.logical_min;
                        f->logical_max = g.logical_max;
                    }
                    offset[g.report_id] = (u16)(offset[g.report_id] + g.report_size);
                }
            } else if (tag == 0x9 || tag == 0xB) {  /* Output / Feature */
                /* Separate report spaces; they do not advance Input offsets. */
            }
            nusages = 0;
            have_range = false;
        }
    }
    return h->nfields;
}

static s32 field_value(const hid_field_t *f, const u8 *r, u32 len)
{
    u32 v = 0;
    for (u8 b = 0; b < f->bit_size; b++) {
        u32 bit = f->bit_offset + b;
        if (bit / 8 >= len) break;
        if (r[bit / 8] & (1u << (bit % 8))) v |= 1u << b;
    }
    if (f->logical_min < 0 && f->bit_size < 32 && (v & (1u << (f->bit_size - 1))))
        v |= ~0u << f->bit_size;                    /* sign-extend */
    return (s32)v;
}

static void screen_size(u32 *w, u32 *h)
{
    struct limine_framebuffer *lfb = az_boot_framebuffer();
    *w = (lfb && lfb->width)  ? (u32)lfb->width  : 1920;
    *h = (lfb && lfb->height) ? (u32)lfb->height : 1080;
}

static void pointer_emit(s32 dx, s32 dy, s32 dz, u8 buttons)
{
    input_event_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.type          = INPUT_EVENT_MOUSE;
    evt.mouse_dx      = (s16)dx;
    evt.mouse_dy      = (s16)dy;
    /* HID wheel is positive away from the user; this kernel's convention
     * (the PS/2 one) is the opposite, as virtio-input.c notes too. */
    evt.mouse_dz      = (s8)-dz;
    evt.mouse_buttons = buttons;
    input_inject(&evt);
}

/* An absolute device's report: the position in screen pixels (see
 * INPUT_EVENT_MOUSE_ABS in input.h). */
static void pointer_emit_abs(s32 x, s32 y, s32 dz, u8 buttons)
{
    input_event_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.type          = INPUT_EVENT_MOUSE_ABS;
    evt.flags         = INPUT_MOUSE_FLAG_BUTTONS;
    evt.mouse_dx      = (s16)x;
    evt.mouse_dy      = (s16)y;
    evt.mouse_dz      = (s8)-dz;
    evt.mouse_buttons = buttons;
    input_inject(&evt);
}

static void pointer_report(usbhid_t *h, const u8 *r, u32 len)
{
    if (h->boot_mouse) {
        if (len < 3) return;
        pointer_emit((s8)r[1], (s8)r[2], len >= 4 ? (s8)r[3] : 0, (u8)(r[0] & 0x1F));
        return;
    }

    u8 id = 0;
    if (h->uses_report_ids) {
        if (len < 1) return;
        id = r[0];
        r++; len--;
    }

    s32 dx = 0, dy = 0, dz = 0;
    u8 buttons = 0;
    bool any = false, abs_x = false, abs_y = false;
    s32 ax = 0, ay = 0;
    u32 w, hgt;
    screen_size(&w, &hgt);

    for (u32 i = 0; i < h->nfields; i++) {
        const hid_field_t *f = &h->fields[i];
        if (f->report_id != id) continue;
        any = true;
        s32 v = field_value(f, r, len);
        if ((f->usage >> 16) == HID_UP_BUTTON) {
            u32 n = f->usage & 0xFFFF;
            if (v && n >= 1 && n <= 5) buttons |= (u8)(1u << (n - 1));
        } else if (f->usage == HID_USAGE_WHEEL) {
            dz += v;
        } else if (f->relative) {
            if (f->usage == HID_USAGE_X) dx += v; else dy += v;
        } else {
            /* Absolute axis: scale the logical range onto the screen. */
            s64 span = (s64)f->logical_max - f->logical_min;
            if (span <= 0) continue;
            u32 extent = f->usage == HID_USAGE_X ? w : hgt;
            s32 pos = (s32)(((s64)(v - f->logical_min) * extent) / span);
            if (f->usage == HID_USAGE_X) { ax = pos; abs_x = true; }
            else                         { ay = pos; abs_y = true; }
        }
    }
    if (!any) return;

    if (abs_x || abs_y) {
        /* An axis a report leaves out keeps its last position. */
        if (abs_x) h->last_abs_x = ax;
        if (abs_y) h->last_abs_y = ay;
        h->have_abs = true;
        pointer_emit_abs(h->last_abs_x, h->last_abs_y, dz, buttons);
        return;
    }
    pointer_emit(dx, dy, dz, buttons);
}

static void usbhid_irq(usb_device_t *dev, void *ctx, const u8 *data, u32 len)
{
    (void)dev;
    usbhid_t *h = (usbhid_t *)ctx;
    if (h->kind == HID_KBD) kbd_report(h, data, len);
    else                    pointer_report(h, data, len);
}

/* ── Class requests ───────────────────────────────────────────────────────── */

static int hid_class_out(usbhid_t *h, u8 req, u16 value, void *data, u16 len)
{
    return usb_control_msg(h->dev, USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
                           req, value, h->iface, data, len);
}

/* The interface's HID descriptor gives the report descriptor's length. */
static u16 hid_report_desc_len(const u8 *p, const u8 *end)
{
    while (p + 2 <= end && p[0] >= 2 && p + p[0] <= end) {
        if (p[1] == USB_DESC_INTERFACE) return 0;       /* next interface */
        if (p[1] == USB_DESC_HID && p[0] >= 9) {
            u8 n = p[5];
            for (u8 i = 0; i < n && 6 + i * 3 + 2 < p[0]; i++)
                if (p[6 + i * 3] == USB_DESC_REPORT)
                    return (u16)(p[7 + i * 3] | (p[8 + i * 3] << 8));
        }
        p += p[0];
    }
    return 0;
}

static const usb_endpoint_descriptor_t *hid_find_int_in(const u8 *p, const u8 *end)
{
    while (p + 2 <= end && p[0] >= 2 && p + p[0] <= end) {
        if (p[1] == USB_DESC_INTERFACE) return NULL;
        if (p[1] == USB_DESC_ENDPOINT && p[0] >= 7) {
            const usb_endpoint_descriptor_t *ep = (const usb_endpoint_descriptor_t *)p;
            if ((ep->bEndpointAddress & USB_ENDPOINT_DIR_IN) &&
                (ep->bmAttributes & USB_ENDPOINT_XFER_MASK) == USB_ENDPOINT_XFER_INT)
                return ep;
        }
        p += p[0];
    }
    return NULL;
}

/* ── Class driver hooks ───────────────────────────────────────────────────── */

static int usbhid_probe(usb_device_t *dev, const usb_interface_descriptor_t *intf,
                        const u8 *cfg, u16 cfg_len, void **priv)
{
    if (intf->bInterfaceClass != USB_CLASS_HID) return -ENODEV;

    const u8 *after = (const u8 *)intf + intf->bLength, *end = cfg + cfg_len;
    const usb_endpoint_descriptor_t *ep = hid_find_int_in(after, end);
    if (!ep) return -ENODEV;

    bool boot = intf->bInterfaceSubClass == HID_SUBCLASS_BOOT;
    bool kbd  = boot && intf->bInterfaceProtocol == HID_PROTO_KEYBOARD;

    usbhid_t *h = (usbhid_t *)kzalloc(sizeof(usbhid_t));
    if (!h) return -ENOMEM;
    h->dev   = dev;
    h->iface = intf->bInterfaceNumber;
    h->kind  = kbd ? HID_KBD : HID_POINTER;

    if (kbd) {
        hid_class_out(h, HID_REQ_SET_PROTOCOL, HID_PROTOCOL_BOOT, NULL, 0);
    } else {
        u16 rlen = hid_report_desc_len(after, end);
        u8 *rdesc = rlen ? (u8 *)kmalloc(rlen) : NULL;
        int n = rdesc ? usb_control_msg(dev, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_INTERFACE,
                                        USB_REQ_GET_DESCRIPTOR, USB_DESC_REPORT << 8,
                                        h->iface, rdesc, rlen) : -ENOMEM;
        bool parsed = n > 0 && hid_parse(h, rdesc, (u32)n) > 0;
        if (rdesc) kfree(rdesc);

        bool has_xy = false;
        for (u32 i = 0; i < h->nfields; i++)
            if (h->fields[i].usage == HID_USAGE_X || h->fields[i].usage == HID_USAGE_Y) has_xy = true;

        if (parsed && has_xy) {
            if (boot) hid_class_out(h, HID_REQ_SET_PROTOCOL, HID_PROTOCOL_REPORT, NULL, 0);
        } else if (boot && intf->bInterfaceProtocol == HID_PROTO_MOUSE) {
            h->boot_mouse = true;
            h->uses_report_ids = false;
            hid_class_out(h, HID_REQ_SET_PROTOCOL, HID_PROTOCOL_BOOT, NULL, 0);
        } else {
            /* A HID interface without pointer axes: a consumer-control or
             * vendor collection this driver has nothing to do with. */
            kfree(h);
            return -ENODEV;
        }
    }

    /* Report only on change (idle rate 0). Optional in the spec, and some
     * devices STALL it — which is harmless, so the result is ignored. */
    hid_class_out(h, HID_REQ_SET_IDLE, 0, NULL, 0);

    int rc = dev->bus->ops->intr_in(dev, ep, usbhid_irq, h);
    if (rc < 0) {
        pr_debug("[USBHID] %04x:%04x: interrupt endpoint setup failed (%d)\n",
                 dev->vendor_id, dev->product_id, rc);
        kfree(h);
        return rc;
    }

    bool relative = true;
    for (u32 i = 0; i < h->nfields; i++)
        if (h->fields[i].usage == HID_USAGE_X) relative = h->fields[i].relative;
    pr_debug("[USBHID] %s '%s' on interface %u (%s)\n",
             kbd ? "keyboard" : "pointer", dev->product[0] ? dev->product : "?", h->iface,
             (kbd || h->boot_mouse) ? "boot protocol" : relative ? "relative" : "absolute");
    *priv = h;
    return 0;
}

static void usbhid_disconnect(usb_device_t *dev, void *priv)
{
    (void)dev;
    usbhid_t *h = (usbhid_t *)priv;
    if (!h) return;
    /* Release anything still held, so a key unplugged mid-press does not
     * stay down in the input subsystem's key-state bitmap forever. */
    if (h->kind == HID_KBD) {
        u8 none[8] = { 0 };
        kbd_report(h, none, sizeof(none));
    } else {
        pointer_emit(0, 0, 0, 0);
    }
    kfree(h);
}

static void usbhid_poll(usb_device_t *dev, void *priv, u64 now_ns)
{
    (void)dev;
    usbhid_t *h = (usbhid_t *)priv;
    if (!h || h->kind != HID_KBD) return;

    /* Typematic repeat, which USB leaves to the host. */
    if (h->repeat_usage) {
        u32 delay_ms, period_ms;
        input_get_keyboard_repeat(&delay_ms, &period_ms);
        if (!delay_ms)  delay_ms  = 500;
        if (!period_ms) period_ms = 33;
        if (h->repeat_next_ns == 0) {
            h->repeat_next_ns = now_ns + (u64)delay_ms * 1000000ULL;
        } else if (now_ns >= h->repeat_next_ns) {
            kbd_key(h->repeat_usage, true);
            h->repeat_next_ns = now_ns + (u64)period_ms * 1000000ULL;
        }
    }

    /* Lock LEDs: whatever the input subsystem says the lock state is. */
    u32 want = input_get_led_state();
    if (!h->leds_known || want != h->leds_shown) {
        /* Boot output report: bit 0 Num, bit 1 Caps, bit 2 Scroll. */
        u8 report = 0;
        if (want & (1U << INPUT_LED_NUMLOCK))    report |= 1;
        if (want & (1U << INPUT_LED_CAPSLOCK))   report |= 2;
        if (want & (1U << INPUT_LED_SCROLLLOCK)) report |= 4;
        hid_class_out(h, HID_REQ_SET_REPORT, 0x0200, &report, 1);
        h->leds_shown = want;
        h->leds_known = true;
    }
}

const usb_class_driver_t usbhid_driver = {
    .name       = "usbhid",
    .probe      = usbhid_probe,
    .disconnect = usbhid_disconnect,
    .poll       = usbhid_poll,
};
