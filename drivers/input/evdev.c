/* ============================================================================
 * AzamiOS — evdev: the Linux input UAPI (/dev/input/event0)
 * File: drivers/input/evdev.c
 *
 * The kernel's own input events are a compact struct carrying a translated
 * keycode and mouse deltas.  evdev is the other convention — the one every
 * Linux input client speaks: a stream of {type, code, value} records with
 * EV_SYN/SYN_REPORT terminating each logical update, plus a set of ioctls
 * for interrogating the device.
 *
 * This is a view onto the same events, not a second consumer of them: the
 * device registers as an input observer, so events reach both the legacy
 * queue that the compositor polls and every open evdev descriptor.  Opening
 * /dev/input/event0 therefore never steals input from anything else.
 *
 * Keycodes need no table: Linux keycodes 1..88 are the AT set-1 scancodes the
 * input layer already carries, so a key event's scancode is its evdev code.
 * ============================================================================ */

#define DEBUG 1
#include <azami/debug.h>
#include "input.h"
#include "../../fs/vfs.h"
#include "../../kernel/uaccess.h"
#include "../../kernel/mm/kmalloc.h"
#include "../../kernel/lib/string.h"
#include "../../arch/x86_64/cpu/spinlock.h"

extern int devfs_register_device(const char *name, file_operations_t *fops, void *private_data);
extern u64 get_cached_unix_time(void);
extern u64 sched_get_ticks(void);

#ifndef POLLIN
#define POLLIN      0x0001
#define POLLNVAL    0x0020
#define POLLRDNORM  0x0040
#endif

/* ── evdev event types and codes ─────────────────────────────────────────── */
#define EV_SYN          0x00
#define EV_KEY          0x01
#define EV_REL          0x02
#define EV_MSC          0x04
#define EV_CNT          0x20

#define SYN_REPORT      0
#define REL_X           0x00
#define REL_Y           0x01
#define REL_WHEEL       0x08
#define MSC_SCAN        0x04

#define BTN_LEFT        0x110
#define BTN_RIGHT       0x111
#define BTN_MIDDLE      0x112
#define BTN_SIDE        0x113
#define BTN_EXTRA       0x114
#define KEY_CNT         0x300
#define REL_CNT         0x10

/* ── ioctls ──────────────────────────────────────────────────────────────── */
#define EVIOCGVERSION   0x80044501
#define EVIOCGID        0x80084502
#define EVIOCGRAB       0x40044590
/* EVIOCGNAME/EVIOCGBIT encode a length in the request, so they are matched on
 * their direction/type/number and the size is read back out of the request. */
#define EVIOC_NR(cmd)   ((cmd) & 0xFF)
#define EVIOC_SIZE(cmd) (((cmd) >> 16) & 0x3FFF)
#define EVDEV_IS_E_REQUEST(cmd) ((((cmd) >> 8) & 0xFF) == 0x45)
#define EVIOCGNAME_NR   0x06
#define EVIOCGPHYS_NR   0x07
#define EVIOCGUNIQ_NR   0x08
#define EVIOCGBIT_BASE  0x20

#define INPUT_PROP_CNT  0x20

#define EVDEV_VERSION   0x010001   /* EV_VERSION, as Linux reports it */

struct input_id {
    u16 bustype, vendor, product, version;
};

/* The wire format: 24 bytes on x86_64. */
struct evdev_event {
    s64 tv_sec;
    s64 tv_usec;
    u16 type;
    u16 code;
    s32 value;
};

#define EVDEV_RING_SIZE 256   /* records buffered per open descriptor */

typedef struct evdev_client {
    struct evdev_event ring[EVDEV_RING_SIZE];
    u32   head, tail;
    bool  used;
} evdev_client_t;

#define EVDEV_MAX_CLIENTS 8
static evdev_client_t g_clients[EVDEV_MAX_CLIENTS];
static spinlock_t     g_evdev_lock = SPINLOCK_INIT;

/* Last reported button state, so a change can be turned into a press/release. */
static u8 g_last_buttons;

/* ── Event fan-out ───────────────────────────────────────────────────────── */

/* Append one record to every open descriptor.  Caller holds g_evdev_lock. */
static void evdev_emit(u16 type, u16 code, s32 value)
{
    u64 ticks = sched_get_ticks();
    struct evdev_event ev;
    ev.tv_sec  = (s64)(ticks / 100);
    ev.tv_usec = (s64)((ticks % 100) * 10000);
    ev.type    = type;
    ev.code    = code;
    ev.value   = value;

    for (u32 i = 0; i < EVDEV_MAX_CLIENTS; i++) {
        evdev_client_t *c = &g_clients[i];
        if (!c->used) continue;

        u32 next = (c->head + 1) % EVDEV_RING_SIZE;
        /* A client that stopped reading loses its oldest records rather than
         * blocking the input path. */
        if (next == c->tail) c->tail = (c->tail + 1) % EVDEV_RING_SIZE;
        c->ring[c->head] = ev;
        c->head = next;
    }
}

/*
 * Translate one kernel input event into an evdev burst.  Runs as an input
 * observer: interrupt context, input's lock held, so it only touches the
 * evdev ring.
 */
static void evdev_observe(const input_event_t *evt)
{
    irqflags_t flags = spinlock_lock_irqsave(&g_evdev_lock);

    bool any = false;

    if (evt->type == INPUT_EVENT_KEY) {
        u8 sc = evt->scancode & 0x7F;
        if (sc) {
            /* MSC_SCAN carries the raw scancode, which is what a client needs
             * to build its own keymap. */
            evdev_emit(EV_MSC, MSC_SCAN, sc);
            evdev_emit(EV_KEY, sc, (evt->flags & KEY_FLAG_RELEASED) ? 0 : 1);
            any = true;
        }
    } else if (evt->type == INPUT_EVENT_MOUSE) {
        if (evt->mouse_dx) { evdev_emit(EV_REL, REL_X, evt->mouse_dx); any = true; }
        if (evt->mouse_dy) { evdev_emit(EV_REL, REL_Y, evt->mouse_dy); any = true; }
        if (evt->mouse_dz) { evdev_emit(EV_REL, REL_WHEEL, evt->mouse_dz); any = true; }

        u8 changed = (u8)(evt->mouse_buttons ^ g_last_buttons);
        static const struct { u8 mask; u16 code; } btns[] = {
            { MOUSE_BTN_LEFT,   BTN_LEFT   },
            { MOUSE_BTN_RIGHT,  BTN_RIGHT  },
            { MOUSE_BTN_MIDDLE, BTN_MIDDLE },
            { MOUSE_BTN_4,      BTN_SIDE   },
            { MOUSE_BTN_5,      BTN_EXTRA  },
        };
        for (u32 i = 0; i < ARRAY_SIZE(btns); i++) {
            if (!(changed & btns[i].mask)) continue;
            evdev_emit(EV_KEY, btns[i].code, (evt->mouse_buttons & btns[i].mask) ? 1 : 0);
            any = true;
        }
        g_last_buttons = evt->mouse_buttons;
    }

    /* Every logical update ends with a synchronisation record. */
    if (any) evdev_emit(EV_SYN, SYN_REPORT, 0);

    spinlock_unlock_irqrestore(&g_evdev_lock, flags);
}

/* ── File operations ─────────────────────────────────────────────────────── */

static s64 evdev_open(inode_t *inode, file_t *filp)
{
    (void)inode;

    irqflags_t flags = spinlock_lock_irqsave(&g_evdev_lock);
    evdev_client_t *c = NULL;
    for (u32 i = 0; i < EVDEV_MAX_CLIENTS; i++) {
        if (!g_clients[i].used) { c = &g_clients[i]; break; }
    }
    if (c) {
        c->used = true;
        c->head = c->tail = 0;
    }
    spinlock_unlock_irqrestore(&g_evdev_lock, flags);

    if (!c) return -(s64)EBUSY;
    filp->private_data = c;
    return 0;
}

static s64 evdev_release(inode_t *inode, file_t *filp)
{
    (void)inode;
    evdev_client_t *c = filp ? (evdev_client_t *)filp->private_data : NULL;
    if (!c) return 0;

    irqflags_t flags = spinlock_lock_irqsave(&g_evdev_lock);
    c->used = false;
    c->head = c->tail = 0;
    spinlock_unlock_irqrestore(&g_evdev_lock, flags);

    filp->private_data = NULL;
    return 0;
}

static s64 evdev_read(file_t *filp, void *buf, size_t len, u64 *offset)
{
    (void)offset;
    evdev_client_t *c = filp ? (evdev_client_t *)filp->private_data : NULL;
    if (!c || !buf) return -(s64)EINVAL;
    /* evdev refuses reads that cannot hold a whole record. */
    if (len < sizeof(struct evdev_event)) return -(s64)EINVAL;

    struct evdev_event staging[32];
    u32 count = 0;

    irqflags_t flags = spinlock_lock_irqsave(&g_evdev_lock);
    while (count < ARRAY_SIZE(staging) &&
           (count + 1) * sizeof(struct evdev_event) <= len &&
           c->tail != c->head) {
        staging[count++] = c->ring[c->tail];
        c->tail = (c->tail + 1) % EVDEV_RING_SIZE;
    }
    spinlock_unlock_irqrestore(&g_evdev_lock, flags);

    if (count == 0) return (filp->f_flags & O_NONBLOCK) ? -(s64)EAGAIN : 0;

    size_t bytes = count * sizeof(struct evdev_event);
    /* Kernel buffer — see fs/vfs.h. */
    memcpy(buf, staging, bytes);
    return (s64)bytes;
}

static int evdev_poll(file_t *filp)
{
    evdev_client_t *c = filp ? (evdev_client_t *)filp->private_data : NULL;
    if (!c) return POLLNVAL;
    return (c->tail != c->head) ? (POLLIN | POLLRDNORM) : 0;
}

/* Set bit @n in a little-endian bitmap of @len bytes. */
static void bitmap_set(u8 *map, size_t len, u32 n)
{
    if (n / 8 < len) map[n / 8] |= (u8)(1u << (n % 8));
}

static s64 evdev_ioctl(file_t *filp, u32 cmd, u64 arg)
{
    (void)filp;
    void *uarg = (void *)(uintptr_t)arg;
    if (!uarg || (uintptr_t)uarg >= 0x8000000000000000ULL) return -(s64)EFAULT;

    switch (cmd) {
    case EVIOCGVERSION: {
        int v = EVDEV_VERSION;
        return copy_to_user(uarg, &v, sizeof(v)) == 0 ? 0 : -(s64)EFAULT;
    }
    case EVIOCGID: {
        /* BUS_HOST = 0x19; the device is the kernel's merged input stream
         * rather than one piece of hardware. */
        struct input_id id = { .bustype = 0x19, .vendor = 0x0001,
                               .product = 0x0001, .version = 0x0100 };
        return copy_to_user(uarg, &id, sizeof(id)) == 0 ? 0 : -(s64)EFAULT;
    }
    case EVIOCGRAB:
        /* Grabbing is exclusive access; every client here already sees the
         * same stream, so there is nothing to take away from anyone. */
        return 0;
    default:
        break;
    }

    if (!EVDEV_IS_E_REQUEST(cmd)) return -(s64)ENOTTY;

    u32 nr   = EVIOC_NR(cmd);
    u32 size = EVIOC_SIZE(cmd);

    if (nr == EVIOCGNAME_NR || nr == EVIOCGPHYS_NR || nr == EVIOCGUNIQ_NR) {
        const char *str = (nr == EVIOCGNAME_NR) ? "AzamiOS Merged Input"
                        : (nr == EVIOCGPHYS_NR) ? "azami/input0" : "";
        size_t l = strlen(str) + 1;
        if (l > size) l = size;
        if (l == 0) return 0;
        if (copy_to_user(uarg, str, l) != 0) return -(s64)EFAULT;
        return (s64)l;
    }

    if (nr >= EVIOCGBIT_BASE && nr < EVIOCGBIT_BASE + EV_CNT) {
        u32 ev = nr - EVIOCGBIT_BASE;
        u8  map[KEY_CNT / 8];
        size_t maplen = size;
        if (maplen > sizeof(map)) maplen = sizeof(map);
        memset(map, 0, sizeof(map));

        if (ev == 0) {
            /* Which event types this device can produce at all. */
            bitmap_set(map, maplen, EV_SYN);
            bitmap_set(map, maplen, EV_KEY);
            bitmap_set(map, maplen, EV_REL);
            bitmap_set(map, maplen, EV_MSC);
        } else if (ev == EV_KEY) {
            /* The whole AT set-1 block, plus the mouse buttons. */
            for (u32 k = 1; k <= 88; k++) bitmap_set(map, maplen, k);
            bitmap_set(map, maplen, BTN_LEFT);
            bitmap_set(map, maplen, BTN_RIGHT);
            bitmap_set(map, maplen, BTN_MIDDLE);
            bitmap_set(map, maplen, BTN_SIDE);
            bitmap_set(map, maplen, BTN_EXTRA);
        } else if (ev == EV_REL) {
            bitmap_set(map, maplen, REL_X);
            bitmap_set(map, maplen, REL_Y);
            bitmap_set(map, maplen, REL_WHEEL);
        } else if (ev == EV_MSC) {
            bitmap_set(map, maplen, MSC_SCAN);
        }

        if (maplen && copy_to_user(uarg, map, maplen) != 0) return -(s64)EFAULT;
        return (s64)maplen;
    }

    return -(s64)ENOTTY;
}

static file_operations_t g_evdev_fops = {
    .read    = evdev_read,
    .ioctl   = evdev_ioctl,
    .open    = evdev_open,
    .release = evdev_release,
    .poll    = evdev_poll,
};

void evdev_init(void)
{
    memset(g_clients, 0, sizeof(g_clients));

    if (input_register_observer(evdev_observe) != 0) {
        pr_debug("[EVDEV] could not register as an input observer\n");
        return;
    }
    devfs_register_device("input/event0", &g_evdev_fops, NULL);
    pr_debug("[EVDEV] Linux input UAPI at /dev/input/event0 (%u concurrent readers)\n",
             (unsigned)EVDEV_MAX_CLIENTS);
}
