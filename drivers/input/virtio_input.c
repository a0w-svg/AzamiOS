/* ============================================================================
 * AzamiOS — virtio-input: keyboards, mice and tablets on the VirtIO bus
 * File: drivers/input/virtio_input.c
 *
 * QEMU's modern input path (`-device virtio-keyboard-pci`, `virtio-mouse-pci`,
 * `virtio-tablet-pci`) delivers Linux evdev records over a virtqueue instead
 * of the PS/2 controller.  Each record is a {type, code, value} triple, and a
 * burst of them is terminated by EV_SYN/SYN_REPORT — so pointer motion is
 * accumulated across a burst and emitted as one event, which is what the
 * kernel's input queue and its consumers expect.
 *
 * Keyboards need no translation table of their own: Linux keycodes 1..88 are
 * the AT set-1 scancodes verbatim, so they go straight back through the
 * shared keymap in input.c and pick up its modifier and lock handling.
 *
 * The device is polled rather than interrupt-driven.  PCI INTx lines are
 * shared and this kernel allows one handler per vector, so claiming the line
 * could silently displace another driver's handler; instead the driver hands
 * the input subsystem a drain callback that runs whenever userspace asks for
 * events, and tells the device not to interrupt at all.
 * ============================================================================ */

#define DEBUG 1
#include <azami/debug.h>
#include "input.h"
#include "../base/pci_bus.h"
#include "../../hal/virtio_pci.h"
#include "../../hal/virtqueue.h"
#include "../../kernel/mm/kmalloc.h"
#include "../../kernel/lib/string.h"
#include "../../arch/x86_64/mm/vmm.h"
#include "../../arch/x86_64/boot/limine_req.h"

/* ── Device configuration space ──────────────────────────────────────────── */
#define VIRTIO_INPUT_CFG_UNSET      0x00
#define VIRTIO_INPUT_CFG_ID_NAME    0x01
#define VIRTIO_INPUT_CFG_ID_DEVIDS  0x03
#define VIRTIO_INPUT_CFG_EV_BITS    0x11
#define VIRTIO_INPUT_CFG_ABS_INFO   0x12

#define VIRTIO_INPUT_CFG_SELECT_OFF 0
#define VIRTIO_INPUT_CFG_SUBSEL_OFF 1
#define VIRTIO_INPUT_CFG_SIZE_OFF   2
#define VIRTIO_INPUT_CFG_DATA_OFF   8

/* ── evdev event types and codes (Linux input-event-codes.h) ─────────────── */
#define EV_SYN          0x00
#define EV_KEY          0x01
#define EV_REL          0x02
#define EV_ABS          0x03

#define SYN_REPORT      0

#define REL_X           0x00
#define REL_Y           0x01
#define REL_WHEEL       0x08

#define ABS_X           0x00
#define ABS_Y           0x01

#define BTN_LEFT        0x110
#define BTN_RIGHT       0x111
#define BTN_MIDDLE      0x112
#define BTN_SIDE        0x113
#define BTN_EXTRA       0x114

#define VIRTIO_INPUT_EVENTQ     0
#define VIRTIO_INPUT_EVENT_SLOTS 32

struct virtio_input_event {
    u16 type;
    u16 code;
    u32 value;
} __packed;

struct virtio_input_absinfo {
    u32 min, max, fuzz, flat, res;
} __packed;

typedef struct virtio_input_device {
    virtio_pci_device_t vpci;
    virtqueue_t        *eventq;
    struct virtio_input_event *slots;   /* VIRTIO_INPUT_EVENT_SLOTS records */

    char   name[64];
    bool   is_keyboard;
    bool   is_pointer;
    bool   is_absolute;

    /* Absolute devices report a position in their own coordinate space; the
     * kernel's event carries deltas, so the previous position is kept here. */
    u32    abs_max_x, abs_max_y;
    s32    last_abs_x, last_abs_y;
    bool   have_last_abs;

    /* Accumulated across one EV_SYN-terminated burst. */
    s32    acc_dx, acc_dy, acc_dz;
    u8     buttons;
    bool   pending;

    struct virtio_input_device *next;
} virtio_input_device_t;

static virtio_input_device_t *g_devices;
static bool g_poll_source_registered;

/* ── Configuration space access ──────────────────────────────────────────── */

static u8 vinput_cfg_select(virtio_input_device_t *vi, u8 select, u8 subsel)
{
    volatile u8 *cfg = vi->vpci.device_cfg;
    if (!cfg) return 0;
    cfg[VIRTIO_INPUT_CFG_SELECT_OFF] = select;
    cfg[VIRTIO_INPUT_CFG_SUBSEL_OFF] = subsel;
    return cfg[VIRTIO_INPUT_CFG_SIZE_OFF];
}

static void vinput_cfg_read(virtio_input_device_t *vi, void *out, u8 len)
{
    volatile u8 *cfg = vi->vpci.device_cfg;
    u8 *dst = (u8 *)out;
    for (u8 i = 0; i < len; i++) {
        dst[i] = cfg[VIRTIO_INPUT_CFG_DATA_OFF + i];
    }
}

/* Does the device report any event of @type?  A zero-length bitmap means no. */
static bool vinput_has_ev(virtio_input_device_t *vi, u8 type)
{
    return vinput_cfg_select(vi, VIRTIO_INPUT_CFG_EV_BITS, type) > 0;
}

/* ── Event translation ───────────────────────────────────────────────────── */

/*
 * Linux keycodes 96..127 sit past the main block and correspond to the
 * 0xE0-prefixed set-1 scancodes.  Zero means "no set-1 equivalent".
 */
static const u8 g_keycode_to_e0[32] = {
    [96 - 96] = 0x1C,  /* KPENTER    */
    [97 - 96] = 0x1D,  /* RIGHTCTRL  */
    [98 - 96] = 0x35,  /* KPSLASH    */
    [99 - 96] = 0x37,  /* SYSRQ      */
    [100 - 96] = 0x38, /* RIGHTALT   */
    [102 - 96] = 0x47, /* HOME       */
    [103 - 96] = 0x48, /* UP         */
    [104 - 96] = 0x49, /* PAGEUP     */
    [105 - 96] = 0x4B, /* LEFT       */
    [106 - 96] = 0x4D, /* RIGHT      */
    [107 - 96] = 0x4F, /* END        */
    [108 - 96] = 0x50, /* DOWN       */
    [109 - 96] = 0x51, /* PAGEDOWN   */
    [110 - 96] = 0x52, /* INSERT     */
    [111 - 96] = 0x53, /* DELETE     */
    [125 - 96] = 0x5B, /* LEFTMETA   */
    [126 - 96] = 0x5C, /* RIGHTMETA  */
    [127 - 96] = 0x5D, /* COMPOSE    */
};

static void vinput_handle_key(virtio_input_device_t *vi, u16 code, u32 value)
{
    /* value 2 is auto-repeat, which the shared keymap treats as another press. */
    bool pressed = value != 0;

    if (code >= BTN_LEFT && code <= BTN_EXTRA) {
        u8 mask = 0;
        switch (code) {
        case BTN_LEFT:   mask = MOUSE_BTN_LEFT;   break;
        case BTN_RIGHT:  mask = MOUSE_BTN_RIGHT;  break;
        case BTN_MIDDLE: mask = MOUSE_BTN_MIDDLE; break;
        case BTN_SIDE:   mask = MOUSE_BTN_4;      break;
        case BTN_EXTRA:  mask = MOUSE_BTN_5;      break;
        default: return;
        }
        if (pressed) vi->buttons |= mask;
        else         vi->buttons &= (u8)~mask;
        vi->pending = true;
        return;
    }

    if (code >= 1 && code <= 88) {
        input_inject_scancode((u8)code, pressed, false);
    } else if (code >= 96 && code < 128) {
        u8 e0 = g_keycode_to_e0[code - 96];
        if (e0) input_inject_scancode(e0, pressed, true);
    }
}

static void vinput_handle_abs(virtio_input_device_t *vi, u16 code, u32 value)
{
    /*
     * Tablets report a position over their own axis range.  Consumers here
     * work in screen pixels and in deltas, so the position is scaled to the
     * boot framebuffer and differentiated against the previous report.  The
     * first report only establishes the origin.
     */
    static u32 s_screen_w, s_screen_h;
    if (s_screen_w == 0) {
        struct limine_framebuffer *lfb = az_boot_framebuffer();
        s_screen_w = (lfb && lfb->width)  ? (u32)lfb->width  : 1920;
        s_screen_h = (lfb && lfb->height) ? (u32)lfb->height : 1080;
    }

    s32 scaled;
    if (code == ABS_X) {
        scaled = vi->abs_max_x ? (s32)(((u64)value * s_screen_w) / vi->abs_max_x) : (s32)value;
        if (vi->have_last_abs) { vi->acc_dx += scaled - vi->last_abs_x; vi->pending = true; }
        vi->last_abs_x = scaled;
    } else if (code == ABS_Y) {
        scaled = vi->abs_max_y ? (s32)(((u64)value * s_screen_h) / vi->abs_max_y) : (s32)value;
        if (vi->have_last_abs) { vi->acc_dy += scaled - vi->last_abs_y; vi->pending = true; }
        vi->last_abs_y = scaled;
    }
}

/* Emit one pointer event for everything accumulated since the last SYN. */
static void vinput_flush(virtio_input_device_t *vi)
{
    vi->have_last_abs = vi->is_absolute;

    if (!vi->pending) return;
    vi->pending = false;

    input_event_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.type          = INPUT_EVENT_MOUSE;
    evt.mouse_dx      = (s16)vi->acc_dx;
    evt.mouse_dy      = (s16)vi->acc_dy;
    evt.mouse_dz      = (s8)vi->acc_dz;
    evt.mouse_buttons = vi->buttons;
    input_inject(&evt);

    vi->acc_dx = vi->acc_dy = vi->acc_dz = 0;
}

static void vinput_process(virtio_input_device_t *vi, const struct virtio_input_event *ev)
{
    switch (ev->type) {
    case EV_KEY:
        vinput_handle_key(vi, ev->code, ev->value);
        break;
    case EV_REL:
        /* Deltas arrive already relative; the wheel is inverted relative to
         * the PS/2 convention this kernel uses. */
        if (ev->code == REL_X)          { vi->acc_dx += (s32)ev->value; vi->pending = true; }
        else if (ev->code == REL_Y)     { vi->acc_dy += (s32)ev->value; vi->pending = true; }
        else if (ev->code == REL_WHEEL) { vi->acc_dz -= (s32)ev->value; vi->pending = true; }
        break;
    case EV_ABS:
        vinput_handle_abs(vi, ev->code, ev->value);
        break;
    case EV_SYN:
        if (ev->code == SYN_REPORT) vinput_flush(vi);
        break;
    default:
        break;
    }
}

/* ── Queue drain ─────────────────────────────────────────────────────────── */

static void vinput_drain_one(virtio_input_device_t *vi)
{
    u32 len = 0;
    void *cookie;

    while ((cookie = virtqueue_get_used(vi->eventq, &len)) != NULL) {
        /* Cookies are slot index + 1 so that NULL stays "nothing used". */
        u32 slot = (u32)(uintptr_t)cookie - 1;
        if (slot < VIRTIO_INPUT_EVENT_SLOTS && len >= sizeof(struct virtio_input_event)) {
            vinput_process(vi, &vi->slots[slot]);
        }

        phys_addr_t phys = vmm_translate(vmm_kernel_space(), (virt_addr_t)&vi->slots[slot]);
        virtqueue_add_buf(vi->eventq, phys, sizeof(struct virtio_input_event), true, cookie);
    }
    virtqueue_kick(vi->eventq);
}

static void vinput_drain_all(void)
{
    for (virtio_input_device_t *vi = g_devices; vi; vi = vi->next) {
        vinput_drain_one(vi);
    }
}

/* ── Probe ───────────────────────────────────────────────────────────────── */

static int vinput_setup_queue(virtio_input_device_t *vi)
{
    vi->eventq = virtio_pci_setup_queue(&vi->vpci, VIRTIO_INPUT_EVENTQ);
    if (!vi->eventq) return -ENODEV;

    /* The driver polls, so ask the device never to raise its interrupt. */
    vi->eventq->avail->flags = VRING_AVAIL_F_NO_INTERRUPT;

    vi->slots = (struct virtio_input_event *)
        kzalloc(sizeof(struct virtio_input_event) * VIRTIO_INPUT_EVENT_SLOTS);
    if (!vi->slots) return -ENOMEM;

    u32 posted = 0;
    for (u32 i = 0; i < VIRTIO_INPUT_EVENT_SLOTS; i++) {
        phys_addr_t phys = vmm_translate(vmm_kernel_space(), (virt_addr_t)&vi->slots[i]);
        if (virtqueue_add_buf(vi->eventq, phys, sizeof(struct virtio_input_event),
                              true, (void *)(uintptr_t)(i + 1)) == 0) {
            posted++;
        }
    }
    if (posted == 0) return -ENOMEM;

    virtqueue_kick(vi->eventq);
    return 0;
}

static void vinput_read_identity(virtio_input_device_t *vi)
{
    u8 len = vinput_cfg_select(vi, VIRTIO_INPUT_CFG_ID_NAME, 0);
    if (len > sizeof(vi->name) - 1) len = sizeof(vi->name) - 1;
    if (len) vinput_cfg_read(vi, vi->name, len);
    vi->name[len] = '\0';
    if (!vi->name[0]) strncpy(vi->name, "virtio-input", sizeof(vi->name) - 1);

    vi->is_keyboard = vinput_has_ev(vi, EV_KEY);
    vi->is_absolute = vinput_has_ev(vi, EV_ABS);
    vi->is_pointer  = vi->is_absolute || vinput_has_ev(vi, EV_REL);

    if (vi->is_absolute) {
        struct virtio_input_absinfo abs;
        if (vinput_cfg_select(vi, VIRTIO_INPUT_CFG_ABS_INFO, ABS_X) >= sizeof(abs)) {
            vinput_cfg_read(vi, &abs, sizeof(abs));
            vi->abs_max_x = abs.max;
        }
        if (vinput_cfg_select(vi, VIRTIO_INPUT_CFG_ABS_INFO, ABS_Y) >= sizeof(abs)) {
            vinput_cfg_read(vi, &abs, sizeof(abs));
            vi->abs_max_y = abs.max;
        }
    }
}

static int vinput_probe(dm_device_t *dm, const pci_device_id_t *id)
{
    (void)id;

    virtio_input_device_t *vi =
        (virtio_input_device_t *)kzalloc(sizeof(virtio_input_device_t));
    if (!vi) return -ENOMEM;

    if (virtio_pci_init_device(dm->hal, &vi->vpci) < 0) {
        kfree(vi);
        return -ENODEV;
    }

    virtio_pci_set_status(&vi->vpci, 0);
    virtio_pci_set_status(&vi->vpci, VIRTIO_CONFIG_S_ACKNOWLEDGE | VIRTIO_CONFIG_S_DRIVER);

    if (!virtio_pci_negotiate_features(&vi->vpci, 0)) {
        virtio_pci_set_status(&vi->vpci, VIRTIO_CONFIG_S_FAILED);
        kfree(vi);
        return -ENODEV;
    }

    vinput_read_identity(vi);

    int ret = vinput_setup_queue(vi);
    if (ret != 0) {
        virtio_pci_set_status(&vi->vpci, VIRTIO_CONFIG_S_FAILED);
        if (vi->slots) kfree(vi->slots);
        kfree(vi);
        return ret;
    }

    virtio_pci_set_status(&vi->vpci,
                          virtio_pci_get_status(&vi->vpci) | VIRTIO_CONFIG_S_DRIVER_OK);

    /* Belt and braces: also mask INTx at the PCI level. */
    pci_device_info_t *info = to_pci_info(dm);
    if (info) {
        u16 cmd = pci_config_read16(info->bus, info->slot, info->func, PCI_COMMAND);
        pci_config_write16(info->bus, info->slot, info->func, PCI_COMMAND,
                           (u16)(cmd | PCI_CMD_INTERRUPT_DIS));
    }

    vi->next  = g_devices;
    g_devices = vi;

    if (!g_poll_source_registered) {
        input_register_poll_source(vinput_drain_all);
        g_poll_source_registered = true;
    }

    dm_set_drvdata(dm, vi);
    pr_debug("[VIRTIO-INPUT] '%s' (%s%s%s) with %u event slots\n",
             vi->name,
             vi->is_keyboard ? "keys" : "",
             (vi->is_keyboard && vi->is_pointer) ? "+" : "",
             vi->is_pointer ? (vi->is_absolute ? "absolute" : "relative") : "",
             VIRTIO_INPUT_EVENT_SLOTS);
    return 0;
}

static void vinput_remove(dm_device_t *dm)
{
    virtio_input_device_t *vi = (virtio_input_device_t *)dm_get_drvdata(dm);
    if (!vi) return;

    virtio_pci_set_status(&vi->vpci, 0);

    virtio_input_device_t **pp = &g_devices;
    while (*pp) {
        if (*pp == vi) { *pp = vi->next; break; }
        pp = &(*pp)->next;
    }
    if (vi->slots) kfree(vi->slots);
    kfree(vi);
}

static const pci_device_id_t vinput_pci_ids[] = {
    { PCI_DEVICE(0x1AF4, 0x1052) },   /* virtio-input (modern) */
    { PCI_DEVICE(0x1AF4, 0x1012) },   /* virtio-input (legacy) */
    { 0 }
};

static pci_driver_t vinput_pci_driver = {
    .drv      = { .name = "virtio_input" },
    .id_table = vinput_pci_ids,
    .probe    = vinput_probe,
    .remove   = vinput_remove,
};

void virtio_input_init(void)
{
    pci_driver_register(&vinput_pci_driver);
}
